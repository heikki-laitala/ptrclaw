#include "agent.hpp"
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

Agent::Agent(std::unique_ptr<Provider> provider,
             std::vector<std::unique_ptr<Tool>> tools,
             const Config& config)
    : provider_(std::move(provider))
    , tools_(std::move(tools))
    , config_(config)
    , model_(config.default_model)
{
    // Create memory backend from config
    memory_ = create_memory(config_);
    wire_memory_tools();

    // Create response cache if enabled
    if (memory_ && config_.memory.response_cache) {
        std::string cache_path = expand_home("~/.ptrclaw/response_cache.json");
        response_cache_ = std::make_unique<ResponseCache>(
            cache_path, config_.memory.cache_ttl, config_.memory.cache_max_entries);
    }
}

void Agent::inject_system_prompt() {
    bool include_tool_desc = !provider_->supports_native_tools();
    bool has_memory = memory_ && memory_->backend_name() != "none";
    std::string prompt = build_system_prompt(tools_, include_tool_desc, has_memory);
    history_.insert(history_.begin(), ChatMessage{Role::System, prompt, {}, {}});
    system_prompt_injected_ = true;
}

std::string Agent::process(const std::string& user_message) {
    if (!system_prompt_injected_) {
        inject_system_prompt();
    }

    // Enrich user message with recalled memory context
    std::string enriched_message = memory_enrich(memory_.get(), user_message,
                                                  config_.memory.recall_limit,
                                                  config_.memory.enrich_depth);
    history_.push_back(ChatMessage{Role::User, enriched_message, {}, {}});

    // Build tool specs
    std::vector<ToolSpec> tool_specs;
    if (provider_->supports_native_tools()) {
        tool_specs.reserve(tools_.size());
        for (const auto& tool : tools_) {
            tool_specs.push_back(tool->spec());
        }
    }

    std::string final_content;
    uint32_t iterations = 0;
    bool stream_started = false;

    // Check response cache before calling provider
    if (response_cache_) {
        std::string sys_prompt;
        if (!history_.empty() && history_[0].role == Role::System) {
            sys_prompt = history_[0].content;
        }
        auto cached = response_cache_->get(model_, sys_prompt, user_message);
        if (cached) {
            history_.push_back(ChatMessage{Role::Assistant, *cached, {}, {}});
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
            if (provider_->supports_streaming()) {
                response = provider_->chat_stream(
                    history_, tool_specs, model_, config_.default_temperature,
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
                                           config_.default_temperature);
            }
        } catch (const std::exception& e) {
            return std::string("Error calling provider: ") + e.what();
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

    // Auto-save user+assistant messages to memory if enabled
    if (memory_ && config_.memory.auto_save && memory_->backend_name() != "none") {
        memory_->store("msg:" + std::to_string(epoch_seconds()),
                       user_message, MemoryCategory::Conversation, session_id_);
        if (!final_content.empty() && final_content != "[Max tool iterations reached]") {
            memory_->store("reply:" + std::to_string(epoch_seconds()),
                           final_content, MemoryCategory::Conversation, session_id_);
        }
    }

    // Populate response cache
    if (response_cache_ && !final_content.empty()) {
        std::string sys_prompt;
        if (!history_.empty() && history_[0].role == Role::System) {
            sys_prompt = history_[0].content;
        }
        response_cache_->put(model_, sys_prompt, user_message, final_content);
    }

    // Synthesize knowledge from conversation
    maybe_synthesize();

    // Auto-compact if needed
    compact_history();

    return final_content;
}

uint32_t Agent::estimated_tokens() const {
    uint32_t total = 0;
    for (const auto& msg : history_) {
        total += estimate_tokens(msg.content);
    }
    return total;
}

void Agent::clear_history() {
    history_.clear();
    system_prompt_injected_ = false;
}

void Agent::set_provider(std::unique_ptr<Provider> provider) {
    provider_ = std::move(provider);
    // Re-inject system prompt on next process() call since tool support may differ
    if (system_prompt_injected_ && !history_.empty()) {
        // Remove the old system prompt
        if (history_[0].role == Role::System) {
            history_.erase(history_.begin());
        }
        system_prompt_injected_ = false;
    }
}

std::string Agent::provider_name() const {
    return provider_->provider_name();
}

void Agent::set_memory(std::unique_ptr<Memory> memory) {
    memory_ = std::move(memory);
    wire_memory_tools();
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

void Agent::maybe_synthesize() {
    if (!memory_ || !config_.memory.synthesis || memory_->backend_name() == "none") return;
    if (config_.memory.synthesis_interval == 0) return;

    turns_since_synthesis_++;
    if (turns_since_synthesis_ < config_.memory.synthesis_interval) return;
    turns_since_synthesis_ = 0;

    // Collect recent user+assistant messages for synthesis
    std::vector<ChatMessage> recent;
    size_t start = (history_.size() > 10) ? history_.size() - 10 : 0;
    for (size_t i = start; i < history_.size(); i++) {
        if (history_[i].role == Role::User || history_[i].role == Role::Assistant) {
            recent.push_back(history_[i]);
        }
    }
    if (recent.empty()) return;

    // Get existing entries for link suggestion context
    auto existing = memory_->list(std::nullopt, 50);

    std::string synthesis_prompt = build_synthesis_prompt(recent, existing);

    try {
        std::string result = provider_->chat_simple(
            synthesis_prompt, "You are a knowledge extraction assistant.",
            model_, 0.3);

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

void Agent::compact_history() {
    uint32_t tokens = estimated_tokens();
    auto threshold = static_cast<uint32_t>(config_.agent.token_limit * 0.75);

    bool should_compact = history_.size() > config_.agent.max_history_messages ||
                          tokens > threshold;

    if (!should_compact || history_.size() <= 12) return;

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

    // Keep last 10 messages
    for (size_t i = history_.size() - 10; i < history_.size(); i++) {
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
