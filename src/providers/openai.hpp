#pragma once
#include "../provider.hpp"
#include "../http.hpp"
#include <nlohmann/json.hpp>
#include <functional>
#include <string>

namespace ptrclaw {

class OpenAIProvider : public Provider {
public:
    OpenAIProvider(const std::string& api_key, HttpClient& http,
                   const std::string& base_url,
                   bool use_oauth = false,
                   const std::string& oauth_access_token = "",
                   const std::string& oauth_refresh_token = "",
                   uint64_t oauth_expires_at = 0,
                   const std::string& oauth_client_id = "",
                   const std::string& oauth_token_url = "");

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
    std::string provider_name() const override { return "openai"; }

    using TokenRefreshCallback = std::function<void(const std::string& access_token,
                                                     const std::string& refresh_token,
                                                     uint64_t expires_at)>;
    void set_on_token_refresh(TokenRefreshCallback cb) { on_token_refresh_ = std::move(cb); }

protected:
    nlohmann::json build_request(const std::vector<ChatMessage>& messages,
                                 const std::vector<ToolSpec>& tools,
                                 const std::string& model,
                                 double temperature) const;
    virtual std::vector<Header> build_headers();
    std::string bearer_token();
    void refresh_oauth_if_needed();
    bool use_responses_api(const std::string& model) const;

private:
    nlohmann::json build_responses_request(
        const std::vector<ChatMessage>& messages,
        const std::vector<ToolSpec>& tools,
        const std::string& model, double temperature) const;

    ChatResponse parse_responses_response(
        const nlohmann::json& resp, const std::string& model) const;

    ChatResponse chat_responses(
        const std::vector<ChatMessage>& messages,
        const std::vector<ToolSpec>& tools,
        const std::string& model, double temperature);

    ChatResponse chat_stream_responses(
        const std::vector<ChatMessage>& messages,
        const std::vector<ToolSpec>& tools,
        const std::string& model, double temperature,
        const TextDeltaCallback& on_delta);

    std::string api_key_;
    HttpClient& http_;
    std::string base_url_;
    bool use_oauth_ = false;
    std::string oauth_access_token_;
    std::string oauth_refresh_token_;
    uint64_t oauth_expires_at_ = 0;
    std::string oauth_client_id_;
    std::string oauth_token_url_;
    TokenRefreshCallback on_token_refresh_;
};

} // namespace ptrclaw
