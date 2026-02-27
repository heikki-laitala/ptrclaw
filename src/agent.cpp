#include "agent.hpp"
#include "embedder.hpp"
#include "memory.hpp"
#include "memory/response_cache.hpp"
#include "event.hpp"
#include "event_bus.hpp"
#include "dispatcher.hpp"
#include "prompt.hpp"
#include "util.hpp"
#include <iostream>
#include <sstream>
#include <nlohmann/json.hpp>

namespace ptrclaw {

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

Agent::Agent(std::unique_ptr<Provider> provider,
             std::vector<std::unique_ptr<Tool>> tools,
             const Config& config)
    : provider_(std::move(provider))
    , tools_(std::move(tools))
    , config_(config)
    , model_(config.model)
{
    // Create memory backend from config
    memory_ = create_memory(config_);
    if (memory_) {
        memory_->set_recency_decay(config_.memory.recency_half_life);
    }
    wire_memory_tools();

    // Create response cache if enabled
    if (memory_ && config_.memory.response_cache) {
        std::string cache_path = expand_home("~/.ptrclaw/response_cache.json");
        response_cache_ = std::make_unique<ResponseCache>(
            cache_path, config_.memory.cache_ttl, config_.memory.cache_max_entries);
    }
}

void Agent::inject_system_prompt() {
    std::string prompt;
    if (hatching_) {
        prompt = build_hatch_prompt();
    } else {
        bool include_tool_desc = !provider_->supports_native_tools();
        bool has_memory = memory_ && memory_->backend_name() != "none";
        RuntimeInfo runtime{model_, provider_->provider_name(), channel_,
                           binary_path_, session_id_};
        prompt = build_system_prompt(tools_, include_tool_desc, has_memory,
                                     memory_.get(), runtime);
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
    has_last_prompt_tokens_ = false;
    last_prompt_tokens_ = 0;
}

std::string Agent::process(const std::string& user_message) {
    if (!system_prompt_injected_) {
        inject_system_prompt();
    }

    // Enrich user message with recalled memory context (skip during hatching)
    std::string enriched_message = user_message;
    if (!hatching_) {
        enriched_message = memory_enrich(memory_.get(), user_message,
                                          config_.memory.recall_limit,
                                          config_.memory.enrich_depth);
    }
    history_.push_back(ChatMessage{Role::User, enriched_message, {}, {}});

    // Build tool specs (skip during hatching — no tools needed for interview)
    std::vector<ToolSpec> tool_specs;
    if (!hatching_ && provider_->supports_native_tools()) {
        tool_specs.reserve(tools_.size());
        for (const auto& tool : tools_) {
            tool_specs.push_back(tool->spec());
        }
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
            has_last_prompt_tokens_ = false;
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
            has_last_prompt_tokens_ = true;
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

        // Execute tool calls
        std::string xml_results;
        for (const auto& call : response.tool_calls) {
            std::cerr << "[tool] " << call.name << '\n';

            // Emit ToolCallRequest event
            if (event_bus_) {
                ToolCallRequestEvent ev;
                ev.session_id = session_id_;
                ev.tool_name = call.name;
                ev.tool_call_id = call.id;
                event_bus_->publish(ev);
            }

            ToolResult result = dispatch_tool(call, tools_);

            // Emit ToolCallResult event
            if (event_bus_) {
                ToolCallResultEvent ev;
                ev.session_id = session_id_;
                ev.tool_name = call.name;
                ev.success = result.success;
                event_bus_->publish(ev);
            }

            if (provider_->supports_native_tools()) {
                history_.push_back(
                    format_tool_result_message(call.id, call.name, result.success, result.output));
            } else {
                xml_results += format_tool_results_xml(call.name, result.success, result.output);
                xml_results += "\n";
            }
        }

        // For non-native providers, append XML results as user message
        if (!provider_->supports_native_tools() && !xml_results.empty()) {
            history_.push_back(ChatMessage{Role::User, xml_results, {}, {}});
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
    if (memory_ && config_.memory.auto_save && memory_->backend_name() != "none") {
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
    if (has_last_prompt_tokens_) {
        return last_prompt_tokens_;
    }

    uint32_t total = 0;
    for (const auto& msg : history_) {
        total += estimate_tokens(msg.content);
    }
    return total;
}

void Agent::clear_history() {
    for (auto& tool : tools_) {
        tool->reset();
    }
    history_.clear();
    system_prompt_injected_ = false;
    has_last_prompt_tokens_ = false;
    last_prompt_tokens_ = 0;
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
        memory_->set_recency_decay(config_.memory.recency_half_life);
    }
    wire_memory_tools();
    if (memory_ && embedder_) {
        memory_->set_embedder(embedder_,
                              config_.memory.embeddings.text_weight,
                              config_.memory.embeddings.vector_weight);
    }
}

void Agent::wire_memory_tools() {
    if (!memory_) return;
    for (auto& tool : tools_) {
        const auto& name = tool->tool_name();
        if (name == "memory_store" || name == "memory_recall" ||
            name == "memory_forget" || name == "memory_link") {
            static_cast<MemoryAwareTool*>(tool.get())->set_memory(memory_.get());
        }
    }
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
    if (!memory_ || !config_.memory.synthesis || memory_->backend_name() == "none") return;

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

            memory_->store(key, content, category, session_id_);

            if (note.contains("links") && note["links"].is_array()) {
                for (const auto& lnk : note["links"]) {
                    if (lnk.is_string()) {
                        memory_->link(key, lnk.get<std::string>());
                    }
                }
            }
        }
    } catch (...) { // NOLINT(bugprone-empty-catch)
        // Synthesis failure is non-critical — silently continue
    }
}

void Agent::maybe_synthesize() {
    if (!memory_ || !config_.memory.synthesis || memory_->backend_name() == "none") return;
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

    // Keep system prompt (first message) + last 10 messages
    // Summarize the middle portion
    std::ostringstream summary;
    summary << "[Conversation history compacted. Previous discussion covered: ";

    size_t start = (history_[0].role == Role::System) ? 1 : 0;
    size_t end = history_.size() - 10;

    int user_count = 0;
    int assistant_count = 0;
    int tool_count = 0;

    for (size_t i = start; i < end; i++) {
        switch (history_[i].role) {
            case Role::User: user_count++; break;
            case Role::Assistant: assistant_count++; break;
            case Role::Tool: tool_count++; break;
            default: break;
        }
    }

    summary << user_count << " user messages, "
            << assistant_count << " assistant responses";
    if (tool_count > 0) {
        summary << ", " << tool_count << " tool calls";
    }
    summary << "]";

    // Build compacted history
    std::vector<ChatMessage> compacted;
    compacted.reserve(12);

    // Keep system prompt
    if (!history_.empty() && history_[0].role == Role::System) {
        compacted.push_back(std::move(history_[0]));
    }

    // Add summary
    compacted.push_back(ChatMessage{Role::User, summary.str(), {}, {}});

    // Keep last 10 messages, but adjust cut point to avoid orphaning tool responses
    size_t keep_from = history_.size() - 10;
    // Walk back if we'd start on a Tool message — include the preceding assistant+tool group
    while (keep_from > 0 && history_[keep_from].role == Role::Tool) {
        keep_from--;
    }
    for (size_t i = keep_from; i < history_.size(); i++) {
        compacted.push_back(std::move(history_[i]));
    }

    history_ = std::move(compacted);
    std::cerr << "[compact] History compacted to " << history_.size() << " messages\n";

    // Run memory hygiene when compaction triggers
    if (memory_ && config_.memory.hygiene_max_age > 0) {
        memory_->hygiene_purge(config_.memory.hygiene_max_age);
    }
}

} // namespace ptrclaw
