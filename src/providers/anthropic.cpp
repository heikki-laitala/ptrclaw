#include "anthropic.hpp"
#include "../http.hpp"
#include <nlohmann/json.hpp>
#include <stdexcept>

using json = nlohmann::json;

namespace ptrclaw {

static std::string role_to_string(Role role) {
    switch (role) {
        case Role::User: return "user";
        case Role::Assistant: return "assistant";
        case Role::Tool: return "tool";
        case Role::System: return "system";
    }
    return "user";
}

AnthropicProvider::AnthropicProvider(const std::string& api_key)
    : api_key_(api_key) {}

ChatResponse AnthropicProvider::chat(const std::vector<ChatMessage>& messages,
                                      const std::vector<ToolSpec>& tools,
                                      const std::string& model,
                                      double temperature) {
    json request;
    request["model"] = model;
    request["max_tokens"] = 4096;
    request["temperature"] = temperature;

    // Extract system messages
    std::string system_text;
    for (const auto& msg : messages) {
        if (msg.role == Role::System) {
            if (!system_text.empty()) {
                system_text += "\n";
            }
            system_text += msg.content;
        }
    }
    if (!system_text.empty()) {
        request["system"] = system_text;
    }

    // Build messages array (non-system)
    json msgs = json::array();
    for (size_t i = 0; i < messages.size(); i++) {
        const auto& msg = messages[i];
        if (msg.role == Role::System) continue;

        json m;
        if (msg.role == Role::Tool) {
            // Collect consecutive Tool messages into a single user message
            json tool_results = json::array();
            while (i < messages.size() && messages[i].role == Role::Tool) {
                json tool_result;
                tool_result["type"] = "tool_result";
                tool_result["tool_use_id"] = messages[i].tool_call_id.value_or("");
                tool_result["content"] = messages[i].content;
                tool_results.push_back(tool_result);
                i++;
            }
            i--; // adjust for outer loop increment
            m["role"] = "user";
            m["content"] = tool_results;
        } else if (msg.role == Role::Assistant && msg.name.has_value()) {
            // Reconstruct tool_use content blocks from serialized tool calls
            m["role"] = "assistant";
            json content_blocks = json::array();
            if (!msg.content.empty()) {
                content_blocks.push_back({{"type", "text"}, {"text", msg.content}});
            }
            try {
                auto tc_arr = json::parse(msg.name.value());
                if (tc_arr.is_array()) {
                    for (const auto& tc : tc_arr) {
                        json tool_use;
                        tool_use["type"] = "tool_use";
                        tool_use["id"] = tc.value("id", "");
                        tool_use["name"] = tc.value("name", "");
                        tool_use["input"] = json::parse(tc.value("arguments", "{}"));
                        content_blocks.push_back(tool_use);
                    }
                }
            } catch (const std::exception&) { // NOLINT(bugprone-empty-catch)
                // name field isn't tool calls JSON — treat as plain assistant message
            }
            m["content"] = content_blocks;
        } else {
            m["role"] = role_to_string(msg.role);
            m["content"] = msg.content;
        }
        msgs.push_back(m);
    }
    request["messages"] = msgs;

    // Build tools array
    if (!tools.empty()) {
        json tools_arr = json::array();
        for (const auto& tool : tools) {
            json t;
            t["name"] = tool.name;
            t["description"] = tool.description;
            t["input_schema"] = json::parse(tool.parameters_json);
            tools_arr.push_back(t);
        }
        request["tools"] = tools_arr;
    }

    // Make HTTP request
    std::vector<Header> headers = {
        {"x-api-key", api_key_},
        {"anthropic-version", API_VERSION},
        {"content-type", "application/json"}
    };

    auto response = http_post(BASE_URL, request.dump(), headers);

    if (response.status_code < 200 || response.status_code >= 300) {
        throw std::runtime_error("Anthropic API error (HTTP " +
            std::to_string(response.status_code) + "): " + response.body);
    }

    auto resp = json::parse(response.body);

    ChatResponse result;
    result.model = resp.value("model", model);

    // Parse content blocks
    if (resp.contains("content") && resp["content"].is_array()) {
        for (const auto& block : resp["content"]) {
            std::string type = block.value("type", "");
            if (type == "text") {
                result.content = block.value("text", "");
            } else if (type == "tool_use") {
                ToolCall tc;
                tc.id = block.value("id", "");
                tc.name = block.value("name", "");
                if (block.contains("input")) {
                    tc.arguments = block["input"].dump();
                }
                result.tool_calls.push_back(std::move(tc));
            }
        }
    }

    // Parse usage
    if (resp.contains("usage")) {
        const auto& usage = resp["usage"];
        result.usage.prompt_tokens = usage.value("input_tokens", 0u);
        result.usage.completion_tokens = usage.value("output_tokens", 0u);
        result.usage.total_tokens = result.usage.prompt_tokens + result.usage.completion_tokens;
    }

    return result;
}

std::string AnthropicProvider::chat_simple(const std::string& system_prompt,
                                            const std::string& message,
                                            const std::string& model,
                                            double temperature) {
    json request;
    request["model"] = model;
    request["max_tokens"] = 4096;
    request["temperature"] = temperature;

    if (!system_prompt.empty()) {
        request["system"] = system_prompt;
    }

    request["messages"] = json::array({
        {{"role", "user"}, {"content", message}}
    });

    std::vector<Header> headers = {
        {"x-api-key", api_key_},
        {"anthropic-version", API_VERSION},
        {"content-type", "application/json"}
    };

    auto response = http_post(BASE_URL, request.dump(), headers);

    if (response.status_code < 200 || response.status_code >= 300) {
        throw std::runtime_error("Anthropic API error (HTTP " +
            std::to_string(response.status_code) + "): " + response.body);
    }

    auto resp = json::parse(response.body);

    if (resp.contains("content") && resp["content"].is_array()) {
        for (const auto& block : resp["content"]) {
            if (block.value("type", "") == "text") {
                return block.value("text", "");
            }
        }
    }

    return "";
}

} // namespace ptrclaw
