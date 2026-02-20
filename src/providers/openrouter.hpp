#pragma once
#include "../provider.hpp"
#include "../http.hpp"
#include <string>

namespace ptrclaw {

class OpenRouterProvider : public Provider {
public:
    OpenRouterProvider(const std::string& api_key, HttpClient& http);

    ChatResponse chat(const std::vector<ChatMessage>& messages,
                      const std::vector<ToolSpec>& tools,
                      const std::string& model,
                      double temperature) override;

    ChatResponse chat_stream(const std::vector<ChatMessage>& messages,
                             const std::vector<ToolSpec>& tools,
                             const std::string& model,
                             double temperature,
                             const TextDeltaCallback& on_delta) override;

    std::string chat_simple(const std::string& system_prompt,
                            const std::string& message,
                            const std::string& model,
                            double temperature) override;

    bool supports_native_tools() const override { return true; }
    bool supports_streaming() const override { return true; }
    std::string provider_name() const override { return "openrouter"; }

private:
    std::string api_key_;
    HttpClient& http_;
    static constexpr const char* BASE_URL = "https://openrouter.ai/api/v1";
};

} // namespace ptrclaw
