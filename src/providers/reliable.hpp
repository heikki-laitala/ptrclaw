#pragma once
#include "../provider.hpp"
#include <string>
#include <vector>
#include <memory>

namespace ptrclaw {

// Wraps multiple providers with retry/fallback logic
class ReliableProvider : public Provider {
public:
    explicit ReliableProvider(std::vector<std::unique_ptr<Provider>> providers,
                              uint32_t max_retries = 3);

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

    bool supports_native_tools() const override;
    bool supports_streaming() const override;
    std::string provider_name() const override { return "reliable"; }

private:
    std::vector<std::unique_ptr<Provider>> providers_;
    uint32_t max_retries_;
};

} // namespace ptrclaw
