#pragma once
#include "../provider.hpp"
#include "../http.hpp"
#include <nlohmann/json.hpp>
#include <functional>
#include <string>

namespace ptrclaw {

// Identifies this client to OpenAI: on the authorize request and on every subscription
// request. Paired with the built-in client_id, so it is not a free label — changing it
// risks the authorize page refusing a value that id is not registered for.
constexpr const char* kOpenAIOriginator = "pi";

// Guards on the OAuth token endpoint. The response is a small JSON document, so anything
// larger is not one; and a token request is part of an interactive login, so it must not sit
// on the default chat timeout.
constexpr long kOAuthTokenTimeoutSeconds = 30;
constexpr std::size_t kOAuthTokenBodyLimitBytes = std::size_t{1024} * 1024;

// Whether a URL is https. A refresh token is a long-lived credential, so there is no
// configuration in which sending one over plaintext is the intended behavior.
bool is_https_url(const std::string& url);

// The ChatGPT account a subscription access token belongs to, read from its
// `https://api.openai.com/auth` claim. Empty when the token is not a JWT, is unreadable, or
// carries no account — the header is then omitted rather than sent blank. Derived from the
// live token rather than stored, so it follows the token across a refresh.
std::string openai_account_id_from_token(const std::string& access_token);

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

    // Set separately rather than as a tenth constructor argument: it is optional, it is
    // orthogonal to authentication, and that parameter list is already at its limit.
    void set_user(const std::string& user) { user_ = user; }

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

    // Adopt the credentials another provider may have rotated since this one was
    // built. Returns false if none are available.
    //
    // Each session gets its own OpenAIProvider, and each snapshots the refresh
    // token at construction. Without this, two sessions whose access tokens
    // expire together both POST the same refresh token: the second is rejected
    // with invalid_grant, and under refresh-token reuse detection that revokes
    // the whole family — including the token the first one just persisted,
    // locking every session out until the operator re-runs /auth.
    using TokenReloadCallback = std::function<bool(std::string& access_token,
                                                   std::string& refresh_token,
                                                   uint64_t& expires_at)>;
    void set_on_token_reload(TokenReloadCallback cb) { on_token_reload_ = std::move(cb); }

protected:
    nlohmann::json build_request(const std::vector<ChatMessage>& messages,
                                 const std::vector<ToolSpec>& tools,
                                 const std::string& model,
                                 double temperature) const;
    virtual std::vector<Header> build_headers();
    std::string bearer_token();
    void refresh_oauth_if_needed();
    bool use_responses_api(const std::string& model) const;
    bool uses_chatgpt_backend() const;
    std::string responses_url() const;

private:
    // OpenAI's `user` field. Empty means omit it; see ProviderEntry::user.
    std::string user_;
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
    TokenReloadCallback  on_token_reload_;
};

} // namespace ptrclaw
