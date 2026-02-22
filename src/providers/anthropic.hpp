#pragma once
#include "../provider.hpp"
#include "../http.hpp"
#include <nlohmann/json.hpp>
#include <string>

namespace ptrclaw {

class AnthropicProvider : public Provider {
public:
    AnthropicProvider(const std::string& api_key, HttpClient& http,
                      const std::string& base_url);

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
    std::string provider_name() const override { return "anthropic"; }

private:
    nlohmann::json build_request(const std::vector<ChatMessage>& messages,
                                 const std::vector<ToolSpec>& tools,
                                 const std::string& model,
                                 double temperature) const;
    std::string api_key_;
    HttpClient& http_;
    std::string base_url_;
    static constexpr const char* API_VERSION = "2023-06-01";
};

} // namespace ptrclaw
