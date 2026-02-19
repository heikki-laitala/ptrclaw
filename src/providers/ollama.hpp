#pragma once
#include "../provider.hpp"
#include "../http.hpp"
#include <string>

namespace ptrclaw {

class OllamaProvider : public Provider {
public:
    OllamaProvider(HttpClient& http, const std::string& base_url = "http://localhost:11434");

    ChatResponse chat(const std::vector<ChatMessage>& messages,
                      const std::vector<ToolSpec>& tools,
                      const std::string& model,
                      double temperature) override;

    std::string chat_simple(const std::string& system_prompt,
                            const std::string& message,
                            const std::string& model,
                            double temperature) override;

    bool supports_native_tools() const override { return false; }
    std::string provider_name() const override { return "ollama"; }

private:
    HttpClient& http_;
    std::string base_url_;
};

} // namespace ptrclaw
