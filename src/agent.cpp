#include "agent.hpp"
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
{}

void Agent::inject_system_prompt() {
    bool include_tool_desc = !provider_->supports_native_tools();
    std::string prompt = build_system_prompt(tools_, include_tool_desc);
    history_.insert(history_.begin(), ChatMessage{Role::System, prompt, {}, {}});
    system_prompt_injected_ = true;
}

std::string Agent::process(const std::string& user_message) {
    if (!system_prompt_injected_) {
        inject_system_prompt();
    }

    // Append user message
    history_.push_back(ChatMessage{Role::User, user_message, {}, {}});

    // Build tool specs
    std::vector<ToolSpec> tool_specs;
    if (provider_->supports_native_tools()) {
        for (const auto& tool : tools_) {
            tool_specs.push_back(tool->spec());
        }
    }

    std::string final_content;
    uint32_t iterations = 0;

    while (iterations < config_.agent.max_tool_iterations) {
        iterations++;

        ChatResponse response;
        try {
            response = provider_->chat(history_, tool_specs, model_, config_.default_temperature);
        } catch (const std::exception& e) {
            return std::string("Error calling provider: ") + e.what();
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

            ToolResult result = dispatch_tool(call, tools_);

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

const std::string& Agent::provider_name() const {
    static std::string name;
    name = provider_->provider_name();
    return name;
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
}

} // namespace ptrclaw
