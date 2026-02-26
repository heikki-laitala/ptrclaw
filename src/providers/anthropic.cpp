#include "anthropic.hpp"
#include "sse.hpp"
#include "../http.hpp"
#include "../plugin.hpp"
#include <nlohmann/json.hpp>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <thread>

static ptrclaw::ProviderRegistrar reg_anthropic("anthropic",
    [](const std::string& key, ptrclaw::HttpClient& http, const std::string& base_url,
       bool prompt_caching, const ptrclaw::ProviderEntry&) {
        return std::make_unique<ptrclaw::AnthropicProvider>(key, http, base_url,
                                                            prompt_caching);
    });

using json = nlohmann::json;

namespace ptrclaw {

AnthropicProvider::AnthropicProvider(const std::string& api_key, HttpClient& http,
                                     const std::string& base_url,
                                     bool prompt_caching)
    : api_key_(api_key), http_(http),
      base_url_(base_url.empty() ? "https://api.anthropic.com/v1" : base_url),
      prompt_caching_enabled_(prompt_caching) {}

bool AnthropicProvider::is_retryable(long status_code) {
    return status_code == 429 || status_code == 408 || status_code == 409 ||
           (status_code >= 500 && status_code < 600);
}

void AnthropicProvider::backoff_sleep(uint32_t attempt) {
    double delay = std::min(INITIAL_DELAY_S * std::pow(2.0, static_cast<double>(attempt)),
                            MAX_DELAY_S);
    auto ms = static_cast<long>(delay * 1000);
    std::cerr << "[anthropic] Rate limited, retrying in " << ms << "ms...\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

json AnthropicProvider::build_request(const std::vector<ChatMessage>& messages,
                                       const std::vector<ToolSpec>& tools,
                                       const std::string& model,
                                       double temperature) const {
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

    return request;
}

ChatResponse AnthropicProvider::chat(const std::vector<ChatMessage>& messages,
                                      const std::vector<ToolSpec>& tools,
                                      const std::string& model,
                                      double temperature) {
    json request = build_request(messages, tools, model, temperature);
    std::string body = request.dump();

    std::vector<Header> headers = {
        {"x-api-key", api_key_},
        {"anthropic-version", API_VERSION},
        {"content-type", "application/json"}
    };
    if (prompt_caching_enabled_) {
        headers.emplace_back("anthropic-beta", "prompt-caching-2024-07-31");
    }

    for (uint32_t attempt = 0; attempt <= MAX_RETRIES; ++attempt) {
        auto response = http_.post(base_url_ + "/messages", body, headers);

        if (response.status_code >= 200 && response.status_code < 300) {
            auto resp = json::parse(response.body);

            ChatResponse result;
            result.model = resp.value("model", model);

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

            if (resp.contains("usage")) {
                const auto& usage = resp["usage"];
                result.usage.prompt_tokens = usage.value("input_tokens", 0u);
                result.usage.completion_tokens = usage.value("output_tokens", 0u);
                result.usage.total_tokens = result.usage.prompt_tokens + result.usage.completion_tokens;
            }

            return result;
        }

        if (is_retryable(response.status_code) && attempt < MAX_RETRIES) {
            backoff_sleep(attempt);
            continue;
        }

        throw std::runtime_error("Anthropic API error (HTTP " +
            std::to_string(response.status_code) + "): " + response.body);
    }

    // Unreachable, but satisfies compiler
    throw std::runtime_error("Anthropic API error: max retries exceeded");
}

ChatResponse AnthropicProvider::chat_stream(const std::vector<ChatMessage>& messages,
                                             const std::vector<ToolSpec>& tools,
                                             const std::string& model,
                                             double temperature,
                                             const TextDeltaCallback& on_delta) {
    json request = build_request(messages, tools, model, temperature);
    request["stream"] = true;
    std::string body = request.dump();

    std::vector<Header> headers = {
        {"x-api-key", api_key_},
        {"anthropic-version", API_VERSION},
        {"content-type", "application/json"}
    };
    if (prompt_caching_enabled_) {
        headers.emplace_back("anthropic-beta", "prompt-caching-2024-07-31");
    }

    for (uint32_t attempt = 0; attempt <= MAX_RETRIES; ++attempt) {
        ChatResponse result;
        result.model = model;
        std::string accumulated_text;

        struct ToolBlock {
            std::string id;
            std::string name;
            std::string arguments;
        };
        std::vector<ToolBlock> tool_blocks;
        int current_block_index = -1;
        bool in_tool_block = false;

        SSEParser parser;
        bool stream_error = false;
        std::string error_body;
        bool got_stream_data = false;

        auto http_response = http_.stream_post_raw(
            base_url_ + "/messages", body, headers,
            [&](const char* data, size_t len) -> bool {
                got_stream_data = true;
                std::string chunk(data, len);
                parser.feed(chunk, [&](const SSEEvent& sse) -> bool {
                    if (sse.event == "error") {
                        stream_error = true;
                        error_body = sse.data;
                        return false;
                    }

                    if (sse.data.empty() || sse.data == "[DONE]") return true;

                    json payload;
                    try {
                        payload = json::parse(sse.data);
                    } catch (...) {
                        return true;
                    }

                    if (sse.event == "message_start") {
                        if (payload.contains("message")) {
                            const auto& msg = payload["message"];
                            result.model = msg.value("model", model);
                            if (msg.contains("usage")) {
                                result.usage.prompt_tokens = msg["usage"].value("input_tokens", 0u);
                            }
                        }
                    } else if (sse.event == "content_block_start") {
                        current_block_index++;
                        if (payload.contains("content_block")) {
                            const auto& block = payload["content_block"];
                            std::string type = block.value("type", "");
                            if (type == "tool_use") {
                                in_tool_block = true;
                                ToolBlock tb;
                                tb.id = block.value("id", "");
                                tb.name = block.value("name", "");
                                tool_blocks.push_back(std::move(tb));
                            } else {
                                in_tool_block = false;
                            }
                        }
                    } else if (sse.event == "content_block_delta") {
                        if (payload.contains("delta")) {
                            const auto& delta = payload["delta"];
                            std::string delta_type = delta.value("type", "");
                            if (delta_type == "text_delta") {
                                std::string text = delta.value("text", "");
                                if (!text.empty()) {
                                    accumulated_text += text;
                                    if (on_delta) {
                                        on_delta(text);
                                    }
                                }
                            } else if (delta_type == "input_json_delta" && !tool_blocks.empty()) {
                                tool_blocks.back().arguments += delta.value("partial_json", "");
                            }
                        }
                    } else if (sse.event == "content_block_stop") {
                        in_tool_block = false;
                    } else if (sse.event == "message_delta") {
                        if (payload.contains("usage")) {
                            result.usage.completion_tokens = payload["usage"].value("output_tokens", 0u);
                        }
                    }

                    return true;
                });
                return !stream_error;
            });

        if (stream_error) {
            throw std::runtime_error("Anthropic streaming error: " + error_body);
        }

        // Check for retryable HTTP errors (429 returns before any stream data)
        if (http_response.status_code != 0 &&
            (http_response.status_code < 200 || http_response.status_code >= 300)) {
            if (!got_stream_data && is_retryable(http_response.status_code) &&
                attempt < MAX_RETRIES) {
                backoff_sleep(attempt);
                continue;
            }
            throw std::runtime_error("Anthropic API error (HTTP " +
                std::to_string(http_response.status_code) + "): " + http_response.body);
        }

        // Assemble result
        if (!accumulated_text.empty()) {
            result.content = accumulated_text;
        }

        for (auto& tb : tool_blocks) {
            ToolCall tc;
            tc.id = std::move(tb.id);
            tc.name = std::move(tb.name);
            tc.arguments = std::move(tb.arguments);
            result.tool_calls.push_back(std::move(tc));
        }

        result.usage.total_tokens = result.usage.prompt_tokens + result.usage.completion_tokens;
        return result;
    }

    throw std::runtime_error("Anthropic API error: max retries exceeded");
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

    std::string body = request.dump();

    std::vector<Header> headers = {
        {"x-api-key", api_key_},
        {"anthropic-version", API_VERSION},
        {"content-type", "application/json"}
    };
    if (prompt_caching_enabled_) {
        headers.emplace_back("anthropic-beta", "prompt-caching-2024-07-31");
    }

    for (uint32_t attempt = 0; attempt <= MAX_RETRIES; ++attempt) {
        auto response = http_.post(base_url_ + "/messages", body, headers);

        if (response.status_code >= 200 && response.status_code < 300) {
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

        if (is_retryable(response.status_code) && attempt < MAX_RETRIES) {
            backoff_sleep(attempt);
            continue;
        }

        throw std::runtime_error("Anthropic API error (HTTP " +
            std::to_string(response.status_code) + "): " + response.body);
    }

    throw std::runtime_error("Anthropic API error: max retries exceeded");
}

} // namespace ptrclaw
