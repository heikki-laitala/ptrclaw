#pragma once
#include "config.hpp"
#include "http.hpp"
#include "oauth.hpp"
#include <memory>
#include <string>

namespace ptrclaw {

class Provider;

// ── OpenAI OAuth constants ───────────────────────────────────────
constexpr const char* kDefaultOAuthClientId = "app_EMoamEEZ73f0CkXaXp7hrann";
constexpr const char* kDefaultRedirectUri = "http://localhost:1455/auth/callback";
constexpr const char* kDefaultTokenUrl = "https://auth.openai.com/oauth/token";
constexpr const char* kDefaultAuthorizeBaseUrl = "https://auth.openai.com/oauth/authorize";
// The model a fresh connection lands on: a dual-route id, so it works on the credential
// just added and keeps working if the API key later becomes the active one.
constexpr const char* kDefaultOAuthModel = "gpt-5.6-sol";

// ── Model to run after connecting ────────────────────────────────
// Keeps the current model when it is an OpenAI model the subscription can serve, so
// connecting while on gpt-5.5 is not a silent downgrade. current_provider is what was
// active before the switch: another provider's model would be kept by a permissive
// oauth_models and then sent to the ChatGPT backend, which cannot serve it. Falls back to
// kDefaultOAuthModel, which the very next turn is guaranteed to be able to use.
std::string oauth_model_after_connect(const std::string& current_provider,
                                      const std::string& current_model,
                                      const ProviderEntry& openai_entry);

// ── Authorize URL builder ────────────────────────────────────────
std::string build_authorize_url(const std::string& client_id,
                                const std::string& redirect_uri,
                                const std::string& code_challenge,
                                const std::string& state);

// ── Start OAuth flow (PKCE + authorize URL) ─────────────────────
OAuthFlowStart start_oauth_flow(const ProviderEntry& openai_entry);

// ── Token exchange ───────────────────────────────────────────────
// Returns error message (empty on success).
std::string exchange_oauth_token(const std::string& code,
                                  const PendingOAuth& pending,
                                  const ProviderEntry& openai_entry,
                                  HttpClient& http,
                                  ProviderEntry& out_entry);

// ── Apply OAuth result (shared between REPL + channel) ──────────
struct OAuthApplyResult {
    bool success = false;
    bool persisted = false;
    std::string error;
    std::unique_ptr<Provider> provider;
};

OAuthApplyResult apply_oauth_result(const std::string& code,
                                     const PendingOAuth& pending,
                                     Config& config,
                                     HttpClient& http);

} // namespace ptrclaw
