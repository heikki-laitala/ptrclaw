#include "openai.hpp"
#include "../http.hpp"
#include <nlohmann/json.hpp>
#include <stdexcept>

using json = nlohmann::json;

namespace ptrclaw {

static std::string role_to_string(Role role) {
    switch (role) {
        case Role::System: return "system";
        case Role::User: return "user";
        case Role::Assistant: return "assistant";
        case Role::Tool: return "tool";
    }
    return "user";
}

OpenAIProvider::OpenAIProvider(const std::string& api_key, HttpClient& http, const std::string& base_url)
    : api_key_(api_key), http_(http), base_url_(base_url) {}

ChatResponse OpenAIProvider::chat(const std::vector<ChatMessage>& messages,
                                   const std::vector<ToolSpec>& tools,
                                   const std::string& model,
                                   double temperature) {
    json request;
    request["model"] = model;
    request["temperature"] = temperature;

    // Build messages array
    json msgs = json::array();
    for (const auto& msg : messages) {
        json m;
        m["role"] = role_to_string(msg.role);
        m["content"] = msg.content;

        if (msg.role == Role::Tool && msg.tool_call_id.has_value()) {
            m["tool_call_id"] = msg.tool_call_id.value();
        }

        // Check if this is an assistant message with tool calls
        // (tool calls are encoded in the message content as JSON for round-tripping)
        if (msg.role == Role::Assistant && msg.name.has_value()) {
            // name field stores serialized tool_calls for assistant messages
            try {
                auto tc_arr = json::parse(msg.name.value());
                if (tc_arr.is_array() && !tc_arr.empty()) {
                    json tool_calls = json::array();
                    for (const auto& tc : tc_arr) {
                        json tool_call;
                        tool_call["id"] = tc.value("id", "");
                        tool_call["type"] = "function";
                        tool_call["function"] = {
                            {"name", tc.value("name", "")},
                            {"arguments", tc.value("arguments", "")}
                        };
                        tool_calls.push_back(tool_call);
                    }
                    m["tool_calls"] = tool_calls;
                }
            } catch (const std::exception&) { // NOLINT(bugprone-empty-catch)
                // name field isn't tool calls JSON — treat as plain assistant message
            }
        }

        msgs.push_back(m);
    }
    request["messages"] = msgs;

    // Build tools array
    if (!tools.empty()) {
        json tools_arr = json::array();
        for (const auto& tool : tools) {
            json t;
            t["type"] = "function";
            t["function"] = {
                {"name", tool.name},
                {"description", tool.description},
                {"parameters", json::parse(tool.parameters_json)}
            };
            tools_arr.push_back(t);
        }
        request["tools"] = tools_arr;
    }

    // Make HTTP request
    std::string url = base_url_ + "/chat/completions";
    std::vector<Header> headers = {
        {"Authorization", "Bearer " + api_key_},
        {"Content-Type", "application/json"}
    };

    auto response = http_.post(url, request.dump(), headers);

    if (response.status_code < 200 || response.status_code >= 300) {
        throw std::runtime_error("OpenAI API error (HTTP " +
            std::to_string(response.status_code) + "): " + response.body);
    }

    auto resp = json::parse(response.body);

    ChatResponse result;
    result.model = resp.value("model", model);

    // Parse choices
    if (resp.contains("choices") && resp["choices"].is_array() && !resp["choices"].empty()) {
        const auto& choice = resp["choices"][0];
        if (choice.contains("message")) {
            const auto& message = choice["message"];

            if (message.contains("content") && !message["content"].is_null()) {
                result.content = message["content"].get<std::string>();
            }

            // Parse tool calls
            if (message.contains("tool_calls") && message["tool_calls"].is_array()) {
                for (const auto& tc : message["tool_calls"]) {
                    ToolCall tool_call;
                    tool_call.id = tc.value("id", "");
                    if (tc.contains("function")) {
                        tool_call.name = tc["function"].value("name", "");
                        tool_call.arguments = tc["function"].value("arguments", "");
                    }
                    result.tool_calls.push_back(std::move(tool_call));
                }
            }
        }
    }

    // Parse usage
    if (resp.contains("usage")) {
        const auto& usage = resp["usage"];
        result.usage.prompt_tokens = usage.value("prompt_tokens", 0u);
        result.usage.completion_tokens = usage.value("completion_tokens", 0u);
        result.usage.total_tokens = usage.value("total_tokens", 0u);
    }

    return result;
}

std::string OpenAIProvider::chat_simple(const std::string& system_prompt,
                                         const std::string& message,
                                         const std::string& model,
                                         double temperature) {
    json request;
    request["model"] = model;
    request["temperature"] = temperature;

    json msgs = json::array();
    if (!system_prompt.empty()) {
        msgs.push_back({{"role", "system"}, {"content", system_prompt}});
    }
    msgs.push_back({{"role", "user"}, {"content", message}});
    request["messages"] = msgs;

    std::string url = base_url_ + "/chat/completions";
    std::vector<Header> headers = {
        {"Authorization", "Bearer " + api_key_},
        {"Content-Type", "application/json"}
    };

    auto response = http_.post(url, request.dump(), headers);

    if (response.status_code < 200 || response.status_code >= 300) {
        throw std::runtime_error("OpenAI API error (HTTP " +
            std::to_string(response.status_code) + "): " + response.body);
    }

    auto resp = json::parse(response.body);

    if (resp.contains("choices") && resp["choices"].is_array() && !resp["choices"].empty()) {
        const auto& choice = resp["choices"][0];
        if (choice.contains("message") && choice["message"].contains("content") &&
            !choice["message"]["content"].is_null()) {
            return choice["message"]["content"].get<std::string>();
        }
    }

    return "";
}

} // namespace ptrclaw
