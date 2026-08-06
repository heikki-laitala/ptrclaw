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
constexpr const char* kDefaultOAuthModel = "gpt-5-codex-mini";
constexpr const char* kDefaultOriginator = "pi";

// ── Model to run after connecting ────────────────────────────────
// Keeps the current model when the subscription can serve it, so connecting OAuth while
// on a model like gpt-5 is not a silent downgrade. Falls back to kDefaultOAuthModel,
// which the very next turn is guaranteed to be able to use.
std::string oauth_model_after_connect(const std::string& current_model,
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
