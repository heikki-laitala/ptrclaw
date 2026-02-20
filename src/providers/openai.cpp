#include "openai.hpp"
#include "sse.hpp"
#include "../http.hpp"
#include "../plugin.hpp"
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <map>

static ptrclaw::ProviderRegistrar reg_openai("openai",
    [](const std::string& key, ptrclaw::HttpClient& http, const std::string&) {
        return std::make_unique<ptrclaw::OpenAIProvider>(key, http);
    });

using json = nlohmann::json;

namespace ptrclaw {

OpenAIProvider::OpenAIProvider(const std::string& api_key, HttpClient& http, const std::string& base_url)
    : api_key_(api_key), http_(http), base_url_(base_url) {}

json OpenAIProvider::build_request(const std::vector<ChatMessage>& messages,
                                    const std::vector<ToolSpec>& tools,
                                    const std::string& model,
                                    double temperature) const {
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

        if (msg.role == Role::Assistant && msg.name.has_value()) {
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
            }
        }

        msgs.push_back(m);
    }
    request["messages"] = msgs;

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

    return request;
}

ChatResponse OpenAIProvider::chat(const std::vector<ChatMessage>& messages,
                                   const std::vector<ToolSpec>& tools,
                                   const std::string& model,
                                   double temperature) {
    json request = build_request(messages, tools, model, temperature);

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

ChatResponse OpenAIProvider::chat_stream(const std::vector<ChatMessage>& messages,
                                          const std::vector<ToolSpec>& tools,
                                          const std::string& model,
                                          double temperature,
                                          const TextDeltaCallback& on_delta) {
    json request = build_request(messages, tools, model, temperature);
    request["stream"] = true;
    request["stream_options"] = {{"include_usage", true}};

    std::string url = base_url_ + "/chat/completions";
    std::vector<Header> headers = {
        {"Authorization", "Bearer " + api_key_},
        {"Content-Type", "application/json"}
    };

    ChatResponse result;
    result.model = model;
    std::string accumulated_text;

    // Accumulate tool calls by index
    std::map<int, ToolCall> tool_call_map;

    SSEParser parser;

    auto http_response = http_.stream_post_raw(
        url, request.dump(), headers,
        [&](const char* data, size_t len) -> bool {
            std::string chunk(data, len);
            parser.feed(chunk, [&](const SSEEvent& sse) -> bool {
                if (sse.data.empty() || sse.data == "[DONE]") return true;

                json payload;
                try {
                    payload = json::parse(sse.data);
                } catch (...) {
                    return true;
                }

                // Model info
                if (payload.contains("model")) {
                    result.model = payload.value("model", model);
                }

                // Parse choices
                if (payload.contains("choices") && payload["choices"].is_array() &&
                    !payload["choices"].empty()) {
                    const auto& choice = payload["choices"][0];
                    if (choice.contains("delta")) {
                        const auto& delta = choice["delta"];

                        // Text content
                        if (delta.contains("content") && !delta["content"].is_null()) {
                            std::string text = delta["content"].get<std::string>();
                            if (!text.empty()) {
                                accumulated_text += text;
                                if (on_delta) {
                                    on_delta(text);
                                }
                            }
                        }

                        // Tool calls
                        if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
                            for (const auto& tc : delta["tool_calls"]) {
                                int idx = tc.value("index", 0);
                                auto& entry = tool_call_map[idx];
                                if (tc.contains("id")) {
                                    entry.id = tc.value("id", "");
                                }
                                if (tc.contains("function")) {
                                    if (tc["function"].contains("name")) {
                                        entry.name = tc["function"].value("name", "");
                                    }
                                    if (tc["function"].contains("arguments")) {
                                        entry.arguments += tc["function"].value("arguments", "");
                                    }
                                }
                            }
                        }
                    }
                }

                // Usage (sent in final chunk with stream_options)
                if (payload.contains("usage") && !payload["usage"].is_null()) {
                    const auto& usage = payload["usage"];
                    result.usage.prompt_tokens = usage.value("prompt_tokens", 0u);
                    result.usage.completion_tokens = usage.value("completion_tokens", 0u);
                    result.usage.total_tokens = usage.value("total_tokens", 0u);
                }

                return true;
            });
            return true;
        });

    if (http_response.status_code != 0 &&
        (http_response.status_code < 200 || http_response.status_code >= 300)) {
        throw std::runtime_error("OpenAI API error (HTTP " +
            std::to_string(http_response.status_code) + ")");
    }

    if (!accumulated_text.empty()) {
        result.content = accumulated_text;
    }

    for (auto& [idx, tc] : tool_call_map) {
        result.tool_calls.push_back(std::move(tc));
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
