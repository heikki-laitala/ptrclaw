#pragma once
#include "../provider.hpp"
#include "../http.hpp"
#include <nlohmann/json.hpp>
#include <string>

namespace ptrclaw {

class AnthropicProvider : public Provider {
public:
    AnthropicProvider(const std::string& api_key, HttpClient& http,
                      const std::string& base_url,
                      bool prompt_caching = false);

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
    static bool is_retryable(long status_code);
    static void backoff_sleep(uint32_t attempt);

    std::string api_key_;
    HttpClient& http_;
    std::string base_url_;
    bool prompt_caching_enabled_ = false;
    static constexpr const char* API_VERSION = "2023-06-01";
    static constexpr uint32_t MAX_RETRIES = 2;
    static constexpr double INITIAL_DELAY_S = 0.5;
    static constexpr double MAX_DELAY_S = 8.0;
};

} // namespace ptrclaw
