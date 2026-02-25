#pragma once
#include "config.hpp"
#include "http.hpp"
#include <memory>
#include <string>
#include <vector>
#include <utility>

namespace ptrclaw {

struct PendingOAuth;
class Provider;

// ── Constants ────────────────────────────────────────────────────
constexpr const char* kDefaultOAuthClientId = "app_EMoamEEZ73f0CkXaXp7hrann";
constexpr const char* kDefaultRedirectUri = "http://localhost:1455/auth/callback";
constexpr const char* kDefaultTokenUrl = "https://auth.openai.com/oauth/token";
constexpr const char* kDefaultAuthorizeBaseUrl = "https://auth.openai.com/oauth/authorize";
constexpr const char* kDefaultOAuthModel = "gpt-5-codex-mini";

// ── URL encoding ─────────────────────────────────────────────────
std::string oauth_url_encode(const std::string& s);

// ── Form encoding ────────────────────────────────────────────────
std::string form_encode(const std::vector<std::pair<std::string, std::string>>& params);

// ── PKCE helpers ─────────────────────────────────────────────────
std::string make_code_verifier();
std::string make_code_challenge_s256(const std::string& verifier);

// ── Authorize URL builder ────────────────────────────────────────
std::string build_authorize_url(const std::string& client_id,
                                const std::string& redirect_uri,
                                const std::string& code_challenge,
                                const std::string& state);

// ── OAuth input parsing ──────────────────────────────────────────
struct ParsedOAuthInput {
    std::string code;
    std::string state;
};
ParsedOAuthInput parse_oauth_input(const std::string& raw_input);

// ── Token exchange ───────────────────────────────────────────────
// Returns error message (empty on success).
std::string exchange_oauth_token(const std::string& code,
                                  const PendingOAuth& pending,
                                  const ProviderEntry& openai_entry,
                                  HttpClient& http,
                                  ProviderEntry& out_entry);

// ── Config persistence ───────────────────────────────────────────
bool persist_openai_oauth(const ProviderEntry& entry);

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
