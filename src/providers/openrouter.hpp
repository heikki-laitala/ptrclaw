#pragma once
#include "../provider.hpp"
#include <string>

namespace ptrclaw {

class OpenRouterProvider : public Provider {
public:
    explicit OpenRouterProvider(const std::string& api_key);

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
    std::string provider_name() const override { return "openrouter"; }

private:
    std::string api_key_;
    static constexpr const char* BASE_URL = "https://openrouter.ai/api/v1";
};

} // namespace ptrclaw
