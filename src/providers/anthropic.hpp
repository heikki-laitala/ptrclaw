#pragma once
#include "../provider.hpp"
#include <string>

namespace ptrclaw {

class AnthropicProvider : public Provider {
public:
    explicit AnthropicProvider(const std::string& api_key);

    ChatResponse chat(const std::vector<ChatMessage>& messages,
                      const std::vector<ToolSpec>& tools,
                      const std::string& model,
                      double temperature) override;

    std::string chat_simple(const std::string& system_prompt,
                            const std::string& message,
                            const std::string& model,
                            double temperature) override;

    bool supports_native_tools() const override { return true; }
    bool supports_streaming() const override { return true; }
    std::string provider_name() const override { return "anthropic"; }

private:
    std::string api_key_;
    static constexpr const char* BASE_URL = "https://api.anthropic.com/v1/messages";
    static constexpr const char* API_VERSION = "2023-06-01";
};

} // namespace ptrclaw
