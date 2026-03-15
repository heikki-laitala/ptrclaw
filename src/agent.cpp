#include "agent.hpp"
#include "embedder.hpp"
#include "memory.hpp"
#include "memory/response_cache.hpp"
#include "event.hpp"
#include "event_bus.hpp"
#include "tool_manager.hpp"
#include "dispatcher.hpp"
#include "prompt.hpp"
#include "skill.hpp"
#include "util.hpp"
#include <iostream>
#include <sstream>
#include <unordered_set>
#include <nlohmann/json.hpp>

namespace ptrclaw {

// Structured summary of a compacted conversation episode
struct EpisodeSummary {
    int user_turns = 0;
    int assistant_turns = 0;
    int tool_calls = 0;
    std::vector<std::string> tools_used; // unique tool names from discarded messages

    std::string format(const std::string& archive_id = "") const {
        std::ostringstream out;
        out << "[Episode summary: " << (user_turns + assistant_turns) << " turns"
            << " (" << user_turns << " user, " << assistant_turns << " assistant)";
        if (tool_calls > 0) {
            out << ", " << tool_calls << " tool call" << (tool_calls != 1 ? "s" : "");
        }
        if (!tools_used.empty()) {
            out << ". Tools: ";
            for (size_t i = 0; i < tools_used.size(); ++i) {
                if (i > 0) out << ", ";
                out << tools_used[i];
            }
        }
        if (!archive_id.empty()) {
            out << ". Archive: " << archive_id;
        }
        out << ".]";
        return out.str();
    }
};

// Build an EpisodeSummary from the slice of history being discarded [start, end)
static EpisodeSummary build_episode_summary(
    const std::vector<ChatMessage>& history, size_t start, size_t end) {

    EpisodeSummary ep;
    std::vector<std::string> seen_tools;

    for (size_t i = start; i < end; i++) {
        switch (history[i].role) {
            case Role::User:      ep.user_turns++;      break;
            case Role::Assistant: ep.assistant_turns++; break;
            case Role::Tool:      ep.tool_calls++;      break;
            default: break;
        }
        // Extract unique tool names from assistant messages carrying tool_calls JSON
        if (history[i].role == Role::Assistant) {
            const std::string tc_json = history[i].name.value_or("");
            if (!tc_json.empty()) {
                try {
                    auto tc_arr = nlohmann::json::parse(tc_json);
                    if (tc_arr.is_array()) {
                        for (const auto& tc : tc_arr) {
                            if (tc.contains("name") && tc["name"].is_string()) {
                                std::string tname = tc["name"].get<std::string>();
                                if (std::find(seen_tools.begin(), seen_tools.end(), tname)
                                        == seen_tools.end()) {
                                    seen_tools.push_back(tname);
                                }
                            }
                        }
                    }
                } catch (...) {} // NOLINT(bugprone-empty-catch)
            }
        }
    }
    ep.tools_used = std::move(seen_tools);
    return ep;
}

// ── Episode archive serialization ────────────────────────────────────────────

static nlohmann::json chat_message_to_json(const ChatMessage& msg) {
    nlohmann::json j;
    j["role"] = role_to_string(msg.role);
    j["content"] = msg.content;
    if (msg.name.has_value()) {
        j["name"] = *msg.name;
    }
    if (msg.tool_call_id.has_value()) {
        j["tool_call_id"] = *msg.tool_call_id;
    }
    return j;
}

static ChatMessage chat_message_from_json(const nlohmann::json& j) {
    ChatMessage msg;
    const std::string role_str = j.value("role", "user");
    if (role_str == "system")         msg.role = Role::System;
    else if (role_str == "assistant") msg.role = Role::Assistant;
    else if (role_str == "tool")      msg.role = Role::Tool;
    else                              msg.role = Role::User;
    msg.content = j.value("content", "");
    if (j.contains("name") && j["name"].is_string()) {
        msg.name = j["name"].get<std::string>();
    }
    if (j.contains("tool_call_id") && j["tool_call_id"].is_string()) {
        msg.tool_call_id = j["tool_call_id"].get<std::string>();
    }
    return msg;
}

static std::string serialize_episodes(const std::vector<EpisodeRecord>& episodes) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& ep : episodes) {
        nlohmann::json j;
        j["id"] = ep.id;
        j["timestamp"] = ep.timestamp;
        j["user_turns"] = ep.user_turns;
        j["assistant_turns"] = ep.assistant_turns;
        j["tool_calls"] = ep.tool_calls;
        j["tools_used"] = ep.tools_used;
        nlohmann::json msgs = nlohmann::json::array();
        for (const auto& msg : ep.messages) {
            msgs.push_back(chat_message_to_json(msg));
        }
        j["messages"] = std::move(msgs);
        arr.push_back(std::move(j));
    }
    return arr.dump();
}

static std::vector<EpisodeRecord> deserialize_episodes(const std::string& json_blob) {
    std::vector<EpisodeRecord> out;
    if (json_blob.empty()) return out;
    try {
        auto arr = nlohmann::json::parse(json_blob);
        if (!arr.is_array()) return out;
        out.reserve(arr.size());
        for (const auto& j : arr) {
            EpisodeRecord ep;
            ep.id              = j.value("id", "");
            ep.timestamp       = j.value("timestamp", uint64_t{0});
            ep.user_turns      = j.value("user_turns", 0);
            ep.assistant_turns = j.value("assistant_turns", 0);
            ep.tool_calls      = j.value("tool_calls", 0);
            if (j.contains("tools_used") && j["tools_used"].is_array()) {
                for (const auto& t : j["tools_used"]) {
                    if (t.is_string()) ep.tools_used.push_back(t.get<std::string>());
                }
            }
            if (j.contains("messages") && j["messages"].is_array()) {
                ep.messages.reserve(j["messages"].size());
                for (const auto& m : j["messages"]) {
                    ep.messages.push_back(chat_message_from_json(m));
                }
            }
            if (!ep.id.empty()) {
                out.push_back(std::move(ep));
            }
        }
    } catch (...) {} // NOLINT(bugprone-empty-catch)
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────

// Strip [Memory context]...[/Memory context] block prepended by memory_enrich()
static std::string strip_memory_context(const std::string& text) {
    const std::string open_tag = "[Memory context]\n";
    const std::string close_tag = "[/Memory context]\n\n";
    if (text.size() < open_tag.size() || text.compare(0, open_tag.size(), open_tag) != 0) return text;
    auto end = text.find(close_tag);
    if (end == std::string::npos) return text;
    size_t content_start = end + close_tag.size();
    if (content_start >= text.size()) return "";
    return text.substr(content_start);
}

bool Agent::has_active_memory() const {
    return memory_ && memory_->backend_name() != "none";
}

Agent::Agent(std::unique_ptr<Provider> provider,
             const Config& config)
    : provider_(std::move(provider))
    , config_(config)
    , model_(config.model)
{
    // Create memory backend from config
    memory_ = create_memory(config_);
    if (memory_) {
        memory_->apply_config(config_.memory);
        restore_episode_archive();
    }

    // Load skills from default directory
    load_skills();

    // Create response cache if enabled
    if (memory_ && config_.memory.response_cache) {
        std::string cache_path = expand_home("~/.ptrclaw/response_cache.json");
        response_cache_ = std::make_unique<ResponseCache>(
            cache_path, config_.memory.cache_ttl, config_.memory.cache_max_entries);
    }
}

Agent::~Agent() {
    if (event_bus_) {
        if (tools_sub_id_) event_bus_->unsubscribe(tools_sub_id_);
        if (skill_sub_id_) event_bus_->unsubscribe(skill_sub_id_);
    }
}

void Agent::inject_system_prompt() {
    std::string prompt;
    if (hatching_) {
        prompt = build_hatch_prompt();
    } else {
        bool include_tool_desc = !provider_->supports_native_tools();
        RuntimeInfo runtime{model_, provider_->provider_name(), channel_,
                           binary_path_, session_id_};
        const auto* active = find_skill(active_skill_name_);
        prompt = build_system_prompt(cached_tool_specs_, include_tool_desc,
                                     has_active_memory(), memory_.get(), runtime);

        // Inject available skills list
        if (!available_skills_.empty()) {
            prompt += "\n## Skills\n"
                      "Available skills (activate with /skill <name>):\n";
            for (const auto& skill : available_skills_) {
                prompt += "- " + skill.name;
                if (!skill.description.empty()) {
                    prompt += ": " + skill.description;
                }
                prompt += "\n";
            }
            prompt += "Use the skill_activate tool to activate a skill mid-conversation.\n\n";
        }

        // Inject active skill prompt
        if (active) {
            prompt += "\n## Active Skill: " + active->name + "\n"
                      + active->prompt + "\n\n";
        }
    }
    history_.insert(history_.begin(), ChatMessage{Role::System, prompt, {}, {}});
    system_prompt_injected_ = true;
}

bool Agent::is_hatched() const {
    return memory_ && memory_->get("soul:identity").has_value();
}

void Agent::start_hatch() {
    hatching_ = true;
    history_.clear();
    system_prompt_injected_ = false;
    last_prompt_tokens_.reset();
}

std::string Agent::process(const std::string& user_message) {
    if (!system_prompt_injected_) {
        inject_system_prompt();
    }

    // Enrich user message with recalled memory context (skip during hatching).
    // Build an episode context line from recent archives so the model knows
    // what compacted history is available (layered context: concepts, observations, episodes).
    std::string enriched_message = user_message;
    if (!hatching_) {
        std::string episode_ctx;
        if (!episode_archives_.empty()) {
            std::ostringstream ep_ss;
            ep_ss << "Past episodes: ";
            size_t ep_start = episode_archives_.size() > 5
                              ? episode_archives_.size() - 5 : 0;
            for (size_t i = ep_start; i < episode_archives_.size(); i++) {
                if (i > ep_start) ep_ss << ", ";
                const auto& ep = episode_archives_[i];
                ep_ss << ep.id << " (" << (ep.user_turns + ep.assistant_turns) << " turns)";
            }
            episode_ctx = ep_ss.str();
        }
        enriched_message = memory_enrich(memory_.get(), user_message,
                                          config_.memory.recall_limit,
                                          config_.memory.enrich_depth,
                                          episode_ctx);
    }
    history_.push_back(ChatMessage{Role::User, enriched_message, {}, {}});

    // Use cached tool specs from ToolsAvailableEvent (skip during hatching)
    std::vector<ToolSpec> tool_specs;
    if (!hatching_ && provider_->supports_native_tools()) {
        tool_specs = cached_tool_specs_;
    }

    std::string final_content;
    uint32_t iterations = 0;
    bool stream_started = false;

    // Check response cache before calling provider.
    // Use enriched_message (includes memory context) so cache key reflects what
    // the LLM actually sees — different memory state produces a different key.
    if (response_cache_) {
        std::string sys_prompt;
        if (!history_.empty() && history_[0].role == Role::System) {
            sys_prompt = history_[0].content;
        }
        auto cached = response_cache_->get(model_, sys_prompt, enriched_message);
        if (cached) {
            history_.push_back(ChatMessage{Role::Assistant, *cached, {}, {}});
            // Cached responses have no provider usage payload — fall back to heuristic.
            last_prompt_tokens_.reset();
            compact_history();
            return *cached;
        }
    }

    while (iterations < config_.agent.max_tool_iterations) {
        iterations++;

        // Emit ProviderRequest event
        if (event_bus_) {
            ProviderRequestEvent ev;
            ev.session_id = session_id_;
            ev.model = model_;
            ev.message_count = history_.size();
            ev.tool_count = tool_specs.size();
            event_bus_->publish(ev);
        }

        ChatResponse response;
        try {
            if (provider_->supports_streaming() && !config_.agent.disable_streaming) {
                response = provider_->chat_stream(
                    history_, tool_specs, model_, config_.temperature,
                    [this, &stream_started](const std::string& delta) -> bool {
                        // During hatching, consume stream silently (no events)
                        if (hatching_) return true;
                        if (event_bus_) {
                            if (!stream_started) {
                                StreamStartEvent ev;
                                ev.session_id = session_id_;
                                ev.model = model_;
                                event_bus_->publish(ev);
                                stream_started = true;
                            }
                            StreamChunkEvent ev;
                            ev.session_id = session_id_;
                            ev.delta = delta;
                            event_bus_->publish(ev);
                        }
                        return true;
                    });
            } else {
                response = provider_->chat(history_, tool_specs, model_,
                                           config_.temperature);
            }
        } catch (const std::exception& e) {
            return std::string("Error calling provider: ") + e.what();
        }

        // Track actual prompt token usage when provider reports it.
        if (response.usage.prompt_tokens > 0) {
            last_prompt_tokens_ = response.usage.prompt_tokens;
        }

        // Emit ProviderResponse event
        if (event_bus_) {
            ProviderResponseEvent ev;
            ev.session_id = session_id_;
            ev.model = model_;
            ev.has_tool_calls = response.has_tool_calls();
            ev.usage = response.usage;
            event_bus_->publish(ev);
        }

        // Append assistant message (encode tool_calls in name field for round-tripping)
        std::optional<std::string> tool_calls_json;
        if (!response.tool_calls.empty()) {
            nlohmann::json tc_arr = nlohmann::json::array();
            for (const auto& tc : response.tool_calls) {
                tc_arr.push_back({
                    {"id", tc.id},
                    {"name", tc.name},
                    {"arguments", tc.arguments}
                });
            }
            tool_calls_json = tc_arr.dump();
        }

        if (response.content) {
            history_.push_back(ChatMessage{Role::Assistant, *response.content, tool_calls_json, {}});
        } else {
            history_.push_back(ChatMessage{Role::Assistant, "", tool_calls_json, {}});
        }

        // If no tool calls, we're done
        if (!response.has_tool_calls()) {
            // Check for XML tool calls in content (for non-native providers)
            if (!provider_->supports_native_tools() && response.content) {
                auto xml_calls = parse_xml_tool_calls(*response.content);
                if (!xml_calls.empty()) {
                    response.tool_calls = std::move(xml_calls);
                } else {
                    final_content = response.content.value_or("[No response]");
                    break;
                }
            } else {
                final_content = response.content.value_or("[No response]");
                break;
            }
        }

        // Dispatch tool calls via EventBus → ToolManager
        std::string xml_results;
        if (event_bus_) {
            std::string batch_id = generate_id();
            BatchCollector collector(batch_id, response.tool_calls.size());
            uint64_t sub_id = subscribe<ToolCallResultEvent>(*event_bus_,
                std::function<void(const ToolCallResultEvent&)>(
                    [&collector](const ToolCallResultEvent& ev) {
                        collector.on_result(ev);
                    }));

            for (const auto& call : response.tool_calls) {
                ToolCallRequestEvent ev;
                ev.session_id = session_id_;
                ev.batch_id = batch_id;
                ev.tool_name = call.name;
                ev.tool_call_id = call.id;
                ev.arguments_json = call.arguments;
                event_bus_->publish(ev);
            }

            auto timeout = std::chrono::seconds(config_.agent.tool_timeout);
            bool completed = collector.wait(timeout);
            event_bus_->unsubscribe(sub_id);

            if (!completed) {
                std::cerr << "[tool] timeout: " << collector.missing()
                          << " tool call(s) did not complete within "
                          << config_.agent.tool_timeout << "s\n";
                // Signal cancellation to running tools
                ToolCallCancelEvent cancel_ev;
                cancel_ev.batch_id = batch_id;
                event_bus_->publish(cancel_ev);
                // Synthesize timeout results for missing calls
                std::unordered_set<std::string> received_ids;
                for (const auto& r : collector.results())
                    received_ids.insert(r.tool_call_id);
                for (const auto& call : response.tool_calls) {
                    if (received_ids.count(call.id) == 0) {
                        ToolCallResultEvent timeout_ev;
                        timeout_ev.batch_id = batch_id;
                        timeout_ev.tool_call_id = call.id;
                        timeout_ev.tool_name = call.name;
                        timeout_ev.success = false;
                        timeout_ev.output = "Tool call timed out after "
                            + std::to_string(config_.agent.tool_timeout) + "s";
                        collector.on_result(timeout_ev);
                    }
                }
            }

            for (const auto& r : collector.results()) {
                if (provider_->supports_native_tools()) {
                    history_.push_back(
                        format_tool_result_message(r.tool_call_id, r.tool_name,
                                                    r.success, r.output));
                } else {
                    xml_results += format_tool_results_xml(r.tool_name,
                                                            r.success, r.output);
                    xml_results += "\n";
                }
            }
        }

        // For non-native providers, append XML results as user message
        if (!provider_->supports_native_tools() && !xml_results.empty()) {
            history_.push_back(ChatMessage{Role::User, xml_results, {}, {}});
        }

        // A tool (e.g. skill_activate) may have invalidated the system prompt.
        // Re-inject before the next provider call so instructions are present.
        if (!system_prompt_injected_) {
            inject_system_prompt();
        }
    }

    if (final_content.empty()) {
        final_content = "[Max tool iterations reached]";
    }

    // Signal stream complete (placeholder already shows the final text)
    if (event_bus_ && stream_started) {
        StreamEndEvent ev;
        ev.session_id = session_id_;
        event_bus_->publish(ev);
    }

    // Soul extraction during hatching
    if (hatching_ && !final_content.empty()) {
        auto parsed = parse_soul_json(final_content);
        if (parsed.found() && memory_) {
            for (const auto& entry : parsed.entries) {
                memory_->store(entry.first, entry.second, MemoryCategory::Core, "");
            }
            // Replace entire response — the LLM's lead-in text before
            // the <soul> block often references the JSON and reads oddly
            // once the block is stripped.
            final_content = "Soul hatched! Your assistant's identity has been saved.";
            // Synthesize knowledge from hatching conversation (user interests, context)
            turns_since_synthesis_ = config_.memory.synthesis_interval;
            maybe_synthesize();

            hatching_ = false;
            history_.clear();
            system_prompt_injected_ = false;
            return final_content;
        }
    }

    // Auto-save user+assistant messages to memory if enabled
    if (has_active_memory() && config_.memory.auto_save) {
        memory_->store("msg:" + std::to_string(epoch_seconds()),
                       user_message, MemoryCategory::Conversation, session_id_);
        if (!final_content.empty() && final_content != "[Max tool iterations reached]") {
            memory_->store("reply:" + std::to_string(epoch_seconds()),
                           final_content, MemoryCategory::Conversation, session_id_);
        }
    }

    // Populate response cache (keyed on enriched message to match lookup above)
    if (response_cache_ && !final_content.empty()) {
        std::string sys_prompt;
        if (!history_.empty() && history_[0].role == Role::System) {
            sys_prompt = history_[0].content;
        }
        response_cache_->put(model_, sys_prompt, enriched_message, final_content);
    }

    // Synthesize knowledge from conversation
    maybe_synthesize();

    // Auto-compact if needed
    compact_history();

    return final_content;
}

uint32_t Agent::estimated_tokens() const {
    // Prefer real provider-reported prompt usage when available.
    if (last_prompt_tokens_) {
        return *last_prompt_tokens_;
    }

    uint32_t total = 0;
    for (const auto& msg : history_) {
        total += estimate_tokens(msg.content);
    }
    return total;
}

void Agent::clear_history() {
    history_.clear();
    system_prompt_injected_ = false;
    last_prompt_tokens_.reset();
}

void Agent::invalidate_system_prompt() {
    if (system_prompt_injected_ && !history_.empty()) {
        if (history_[0].role == Role::System) {
            history_.erase(history_.begin());
        }
        system_prompt_injected_ = false;
    }
}

void Agent::set_model(const std::string& model) {
    model_ = model;
    invalidate_system_prompt();
}

void Agent::set_provider(std::unique_ptr<Provider> provider) {
    provider_ = std::move(provider);
    invalidate_system_prompt();
}

std::string Agent::provider_name() const {
    return provider_->provider_name();
}

void Agent::set_memory(std::unique_ptr<Memory> memory) {
    memory_ = std::move(memory);
    if (memory_) {
        memory_->apply_config(config_.memory);
        restore_episode_archive();
    }
    if (memory_ && embedder_) {
        memory_->set_embedder(embedder_,
                              config_.memory.embeddings.text_weight,
                              config_.memory.embeddings.vector_weight);
    }
}

void Agent::on_tools_available(const std::vector<ToolSpec>& specs) {
    cached_tool_specs_ = specs;
}

void Agent::set_event_bus(EventBus* bus) {
    // Unsubscribe from previous bus if any
    if (event_bus_) {
        if (tools_sub_id_) event_bus_->unsubscribe(tools_sub_id_);
        if (skill_sub_id_) event_bus_->unsubscribe(skill_sub_id_);
        tools_sub_id_ = 0;
        skill_sub_id_ = 0;
    }
    event_bus_ = bus;
    if (event_bus_) {
        tools_sub_id_ = subscribe<ToolsAvailableEvent>(*event_bus_,
            std::function<void(const ToolsAvailableEvent&)>(
                [this](const ToolsAvailableEvent& ev) {
                    // In multi-session mode, only accept specs for our session
                    if (!session_id_.empty() && !ev.session_id.empty() &&
                        ev.session_id != session_id_) return;
                    on_tools_available(ev.specs);
                }));
        skill_sub_id_ = subscribe<SkillRequestEvent>(*event_bus_,
            std::function<void(const SkillRequestEvent&)>(
                [this](const SkillRequestEvent& ev) {
                    // In multi-session mode, only handle our session's requests
                    if (!session_id_.empty() && !ev.session_id.empty() &&
                        ev.session_id != session_id_) return;
                    on_skill_request(ev);
                }));
    }
}

void Agent::on_skill_request(const SkillRequestEvent& req) {
    if (!event_bus_) return;

    load_skills(); // re-scan for new/changed skills

    SkillResponseEvent resp;
    resp.request_id = req.request_id;

    if (req.action == "deactivate") {
        deactivate_skill();
        resp.success = true;
        resp.message = "Skill deactivated";
    } else if (req.action == "activate") {
        if (activate_skill(req.name)) {
            resp.success = true;
            resp.message = "Skill '" + req.name + "' activated";
        } else {
            resp.success = false;
            std::string available;
            for (const auto& s : available_skills_) {
                if (!available.empty()) available += ", ";
                available += s.name;
            }
            resp.message = "Unknown skill: " + req.name + ". Available: " + available;
        }
    }

    event_bus_->publish(resp);
}

void Agent::set_response_cache(std::unique_ptr<ResponseCache> cache) {
    response_cache_ = std::move(cache);
}

void Agent::set_embedder(Embedder* embedder) {
    embedder_ = embedder;
    if (memory_ && embedder_) {
        memory_->set_embedder(embedder_,
                              config_.memory.embeddings.text_weight,
                              config_.memory.embeddings.vector_weight);
    }
}

void Agent::run_synthesis() {
    if (!has_active_memory() || !config_.memory.synthesis) return;

    // Collect recent user+assistant messages for synthesis
    // Strip [Memory context] blocks from user messages so the synthesis LLM
    // sees only the original user text (avoids duplicate/contaminated context).
    std::vector<ChatMessage> recent;
    size_t start = (history_.size() > 10) ? history_.size() - 10 : 0;
    for (size_t i = start; i < history_.size(); i++) {
        if (history_[i].role == Role::User || history_[i].role == Role::Assistant) {
            ChatMessage msg = history_[i];
            if (msg.role == Role::User) {
                msg.content = strip_memory_context(msg.content);
            }
            recent.push_back(std::move(msg));
        }
    }
    if (recent.empty()) return;

    // Get existing entries for link suggestion context
    auto existing = memory_->list(std::nullopt, 50);

    std::string synthesis_prompt = build_synthesis_prompt(recent, existing);

    try {
        std::string result = provider_->chat_simple(
            "You are a knowledge extraction assistant.",
            synthesis_prompt, model_, 0.3);

        // Parse JSON array response
        auto j = nlohmann::json::parse(result);
        if (!j.is_array()) return;

        for (const auto& note : j) {
            if (!note.contains("key") || !note.contains("content")) continue;

            std::string key = note["key"].get<std::string>();
            std::string content = note["content"].get<std::string>();
            std::string cat_str = note.value("category", "knowledge");
            MemoryCategory category = category_from_string(cat_str);

            // Concepts and core entries are cross-session — stored without session_id
            // so they survive past the current session and are not tied to a single episode.
            // Observations are episode-specific — bound to the current session.
            std::string type_str = note.value("type", "observation");
            bool is_concept = (type_str == "concept") || (category == MemoryCategory::Core);
            std::string entry_session = is_concept ? "" : session_id_;

            memory_->store(key, content, category, entry_session);

            if (note.contains("links") && note["links"].is_array()) {
                for (const auto& lnk : note["links"]) {
                    if (lnk.is_string()) {
                        memory_->link(key, lnk.get<std::string>());
                    }
                }
            }

            // Replacement / contradiction handling (PER-394):
            // When synthesis signals that a new concept supersedes a different existing
            // concept, migrate the old entry's graph links to the new entry and remove
            // the outdated entry so contradictions don't accumulate in the store.
            if (note.contains("replaces") && note["replaces"].is_string()) {
                std::string old_key = note["replaces"].get<std::string>();
                if (!old_key.empty() && old_key != key) {
                    auto old_entry = memory_->get(old_key);
                    if (old_entry) {
                        for (const auto& linked_key : old_entry->links) {
                            if (linked_key != key) {
                                memory_->link(key, linked_key);
                            }
                        }
                        memory_->forget(old_key);
                    }
                }
            }
        }
    } catch (...) { // NOLINT(bugprone-empty-catch)
        // Synthesis failure is non-critical — silently continue
    }
}

void Agent::maybe_synthesize() {
    if (!has_active_memory() || !config_.memory.synthesis) return;
    if (config_.memory.synthesis_interval == 0) return;

    turns_since_synthesis_++;
    if (turns_since_synthesis_ < config_.memory.synthesis_interval) return;
    turns_since_synthesis_ = 0;

    run_synthesis();
}

void Agent::compact_history() {
    uint32_t tokens = estimated_tokens();
    auto threshold = static_cast<uint32_t>(config_.agent.token_limit * 0.75);

    bool should_compact = history_.size() > config_.agent.max_history_messages ||
                          tokens > threshold;

    if (!should_compact || history_.size() <= 12) return;

    // Force synthesis before compaction if there are unsynthesized turns
    if (turns_since_synthesis_ > 0) {
        run_synthesis();
        turns_since_synthesis_ = 0;
    }

    // Keep system prompt (first message) + last 10 messages.
    // Compute keep_from first (with tool-orphan walkback) so the archived range
    // is exactly [start, keep_from) — the messages truly being discarded.
    size_t start = (history_[0].role == Role::System) ? 1 : 0;
    size_t keep_from = history_.size() - 10;
    // Walk back if we'd start on a Tool message — include the preceding assistant+tool group
    while (keep_from > 0 && history_[keep_from].role == Role::Tool) {
        keep_from--;
    }

    EpisodeSummary ep = build_episode_summary(history_, start, keep_from);

    // Archive the dropped slice before it is discarded (PER-388).
    // Episodes are kept in episode_archives_ — separate from knowledge/concept memory.
    EpisodeRecord record;
    record.id = "episode:" + std::to_string(episode_counter_++);
    record.timestamp = epoch_seconds();
    record.user_turns = ep.user_turns;
    record.assistant_turns = ep.assistant_turns;
    record.tool_calls = ep.tool_calls;
    record.tools_used = ep.tools_used;
    record.messages = std::vector<ChatMessage>(
        history_.begin() + static_cast<ptrdiff_t>(start),
        history_.begin() + static_cast<ptrdiff_t>(keep_from));
    episode_archives_.push_back(std::move(record));
    persist_episode_archive();

    // Build compacted history
    std::vector<ChatMessage> compacted;
    compacted.reserve(12);

    // Keep system prompt
    if (!history_.empty() && history_[0].role == Role::System) {
        compacted.push_back(std::move(history_[0]));
    }

    // Add structured episode summary with archive reference
    compacted.push_back(ChatMessage{Role::User, ep.format(episode_archives_.back().id), {}, {}});

    for (size_t i = keep_from; i < history_.size(); i++) {
        compacted.push_back(std::move(history_[i]));
    }

    history_ = std::move(compacted);
    std::cerr << "[compact] History compacted to " << history_.size() << " messages"
              << " (" << episode_archives_.back().id << " archived, "
              << episode_archives_.back().messages.size() << " msgs)\n";

    // Run memory hygiene when compaction triggers
    if (memory_ && config_.memory.hygiene_max_age > 0) {
        memory_->hygiene_purge(config_.memory.hygiene_max_age);
    }
}

const EpisodeRecord* Agent::episode_by_id(const std::string& id) const {
    for (const auto& ep : episode_archives_) {
        if (ep.id == id) return &ep;
    }
    return nullptr;
}

void Agent::persist_episode_archive() {
    if (!memory_) return;
    memory_->save_episode_archive(serialize_episodes(episode_archives_));
}

void Agent::restore_episode_archive() {
    if (!memory_) return;
    const std::string blob = memory_->load_episode_archive();
    if (blob.empty()) return;
    auto loaded = deserialize_episodes(blob);
    if (loaded.empty()) return;
    episode_archives_ = std::move(loaded);
    // Reset counter then advance past highest episode number so new archives don't collide.
    // Always reset (not just advance) so that replacing memory via set_memory() correctly
    // reflects the new archive's numbering rather than the old counter.
    episode_counter_ = 0;
    for (const auto& ep : episode_archives_) {
        // id format: "episode:N"
        const std::string prefix = "episode:";
        if (ep.id.size() > prefix.size() && ep.id.compare(0, prefix.size(), prefix) == 0) {
            try {
                uint32_t n = static_cast<uint32_t>(std::stoul(ep.id.substr(prefix.size())));
                if (n + 1 > episode_counter_) {
                    episode_counter_ = n + 1;
                }
            } catch (...) {} // NOLINT(bugprone-empty-catch)
        }
    }
}

const SkillDef* Agent::find_skill(const std::string& name) const {
    for (const auto& skill : available_skills_) {
        if (skill.name == name) return &skill;
    }
    return nullptr;
}

void Agent::load_skills(const std::string& dir) {
    std::string skills_dir = dir.empty() ? default_skills_dir() : dir;
    auto fresh = ptrclaw::load_skills(skills_dir);
    if (fresh.size() != available_skills_.size()) {
        std::cerr << "[skills] Loaded " << fresh.size()
                  << " skills from " << skills_dir << "\n";
    }
    available_skills_ = std::move(fresh);
}

bool Agent::activate_skill(const std::string& name) {
    if (!find_skill(name)) return false;
    active_skill_name_ = name;
    invalidate_system_prompt();
    return true;
}

void Agent::deactivate_skill() {
    if (!active_skill_name_.empty()) {
        active_skill_name_.clear();
        invalidate_system_prompt();
    }
}

} // namespace ptrclaw
