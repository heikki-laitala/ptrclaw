#include "ollama.hpp"
#include "../http.hpp"
#include "../plugin.hpp"
#include <nlohmann/json.hpp>
#include <stdexcept>

static ptrclaw::ProviderRegistrar reg_ollama("ollama",
    [](const std::string&, ptrclaw::HttpClient& http, const std::string& base_url) {
        std::string url = base_url.empty() ? "http://localhost:11434" : base_url;
        return std::make_unique<ptrclaw::OllamaProvider>(http, url);
    });

using json = nlohmann::json;

namespace ptrclaw {

OllamaProvider::OllamaProvider(HttpClient& http, const std::string& base_url)
    : http_(http), base_url_(base_url) {}

ChatResponse OllamaProvider::chat(const std::vector<ChatMessage>& messages,
                                   const std::vector<ToolSpec>& /* tools */,
                                   const std::string& model,
                                   double /* temperature */) {
    json request;
    request["model"] = model;
    request["stream"] = false;

    json msgs = json::array();
    for (const auto& msg : messages) {
        json m;
        m["role"] = (msg.role == Role::Tool) ? "user" : role_to_string(msg.role);
        m["content"] = msg.content;
        msgs.push_back(m);
    }
    request["messages"] = msgs;

    std::string url = base_url_ + "/api/chat";
    std::vector<Header> headers = {
        {"Content-Type", "application/json"}
    };

    auto response = http_.post(url, request.dump(), headers);

    if (response.status_code < 200 || response.status_code >= 300) {
        throw std::runtime_error("Ollama API error (HTTP " +
            std::to_string(response.status_code) + "): " + response.body);
    }

    auto resp = json::parse(response.body);

    ChatResponse result;
    result.model = resp.value("model", model);

    if (resp.contains("message") && resp["message"].contains("content")) {
        result.content = resp["message"]["content"].get<std::string>();
    }

    // Estimate token usage from response
    if (resp.contains("prompt_eval_count")) {
        result.usage.prompt_tokens = resp.value("prompt_eval_count", 0u);
    }
    if (resp.contains("eval_count")) {
        result.usage.completion_tokens = resp.value("eval_count", 0u);
    }
    result.usage.total_tokens = result.usage.prompt_tokens + result.usage.completion_tokens;

    return result;
}

std::string OllamaProvider::chat_simple(const std::string& system_prompt,
                                         const std::string& message,
                                         const std::string& model,
                                         double temperature) {
    std::vector<ChatMessage> messages;
    if (!system_prompt.empty()) {
        messages.push_back({Role::System, system_prompt, std::nullopt, std::nullopt});
    }
    messages.push_back({Role::User, message, std::nullopt, std::nullopt});

    auto result = chat(messages, {}, model, temperature);
    return result.content.value_or("");
}

} // namespace ptrclaw
