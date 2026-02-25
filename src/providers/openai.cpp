#include "openai.hpp"
#include "sse.hpp"
#include "../http.hpp"
#include "../plugin.hpp"
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <map>
#include <chrono>

static ptrclaw::ProviderRegistrar reg_openai("openai",
    [](const std::string& key, ptrclaw::HttpClient& http, const std::string& base_url,
       bool /* prompt_caching */, const ptrclaw::ProviderEntry& entry) {
        return std::make_unique<ptrclaw::OpenAIProvider>(
            key,
            http,
            base_url,
            entry.use_oauth,
            entry.oauth_access_token,
            entry.oauth_refresh_token,
            entry.oauth_expires_at,
            entry.oauth_client_id,
            entry.oauth_token_url);
    });

using json = nlohmann::json;

namespace ptrclaw {

OpenAIProvider::OpenAIProvider(const std::string& api_key, HttpClient& http,
                               const std::string& base_url,
                               bool use_oauth,
                               const std::string& oauth_access_token,
                               const std::string& oauth_refresh_token,
                               uint64_t oauth_expires_at,
                               const std::string& oauth_client_id,
                               const std::string& oauth_token_url)
    : api_key_(api_key), http_(http),
      base_url_(base_url.empty() ? "https://api.openai.com/v1" : base_url),
      use_oauth_(use_oauth),
      oauth_access_token_(oauth_access_token),
      oauth_refresh_token_(oauth_refresh_token),
      oauth_expires_at_(oauth_expires_at),
      oauth_client_id_(oauth_client_id.empty() ? "openai-codex" : oauth_client_id),
      oauth_token_url_(oauth_token_url.empty()
                       ? "https://auth.openai.com/oauth/token"
                       : oauth_token_url) {}

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

std::string OpenAIProvider::bearer_token() {
    if (use_oauth_) {
        refresh_oauth_if_needed();
        return oauth_access_token_;
    }
    return api_key_;
}

void OpenAIProvider::refresh_oauth_if_needed() {
    if (!use_oauth_) return;

    auto now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    // Keep 60s safety buffer.
    bool expired_or_missing = oauth_access_token_.empty() ||
        (oauth_expires_at_ > 0 && now + 60 >= oauth_expires_at_);

    if (!expired_or_missing) return;
    if (oauth_refresh_token_.empty()) {
        throw std::runtime_error("OpenAI OAuth access token expired and no refresh token is configured");
    }

    json refresh_req;
    refresh_req["grant_type"] = "refresh_token";
    refresh_req["refresh_token"] = oauth_refresh_token_;
    if (!oauth_client_id_.empty()) {
        refresh_req["client_id"] = oauth_client_id_;
    }

    auto refresh_resp = http_.post(
        oauth_token_url_,
        refresh_req.dump(),
        {{"Content-Type", "application/json"}});

    if (refresh_resp.status_code < 200 || refresh_resp.status_code >= 300) {
        throw std::runtime_error("OpenAI OAuth refresh failed (HTTP " +
            std::to_string(refresh_resp.status_code) + "): " + refresh_resp.body);
    }

    auto token_json = json::parse(refresh_resp.body);
    oauth_access_token_ = token_json.value("access_token", "");
    if (oauth_access_token_.empty()) {
        throw std::runtime_error("OpenAI OAuth refresh response missing access_token");
    }

    auto expires_in = token_json.value("expires_in", 3600u);
    oauth_expires_at_ = now + static_cast<uint64_t>(expires_in);

    // Some providers rotate refresh token.
    std::string new_refresh = token_json.value("refresh_token", "");
    if (!new_refresh.empty()) {
        oauth_refresh_token_ = new_refresh;
    }

    if (on_token_refresh_) {
        on_token_refresh_(oauth_access_token_, oauth_refresh_token_, oauth_expires_at_);
    }
}

std::vector<Header> OpenAIProvider::build_headers() {
    std::string token = bearer_token();
    return {
        {"Authorization", "Bearer " + token},
        {"Content-Type", "application/json"}
    };
}

ChatResponse OpenAIProvider::chat(const std::vector<ChatMessage>& messages,
                                   const std::vector<ToolSpec>& tools,
                                   const std::string& model,
                                   double temperature) {
    json request = build_request(messages, tools, model, temperature);

    std::string url = base_url_ + "/chat/completions";
    auto headers = build_headers();

    auto response = http_.post(url, request.dump(), headers);

    if (response.status_code < 200 || response.status_code >= 300) {
        throw std::runtime_error(provider_name() + " API error (HTTP " +
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
    auto headers = build_headers();

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
        throw std::runtime_error(provider_name() + " API error (HTTP " +
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
    auto headers = build_headers();

    auto response = http_.post(url, request.dump(), headers);

    if (response.status_code < 200 || response.status_code >= 300) {
        throw std::runtime_error(provider_name() + " API error (HTTP " +
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
