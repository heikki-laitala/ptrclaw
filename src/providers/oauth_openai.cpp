#include "providers/oauth_openai.hpp"
#include "providers/openai_token_persist.hpp"
#include "provider.hpp"
#include "providers/openai.hpp"
#include "util.hpp"

#include <nlohmann/json.hpp>

namespace ptrclaw {

using json = nlohmann::json;

// ── Model to run after connecting ────────────────────────────────

std::string oauth_model_after_connect(const std::string& current_provider,
                                      const std::string& current_model,
                                      const ProviderEntry& openai_entry) {
    if (current_provider == "openai" &&
        openai_oauth_eligible(current_model, openai_entry)) {
        return current_model;
    }
    return kDefaultOAuthModel;
}

// ── Authorize URL builder ────────────────────────────────────────

std::string build_authorize_url(const std::string& client_id,
                                const std::string& redirect_uri,
                                const std::string& code_challenge,
                                const std::string& state) {
    // Use '+' for spaces (application/x-www-form-urlencoded) to match
    // JavaScript URLSearchParams behavior used by working implementations.
    std::string scope = "openid+profile+email+offline_access";
    return std::string(kDefaultAuthorizeBaseUrl) +
        "?response_type=code"
        "&client_id=" + url_encode(client_id) +
        "&redirect_uri=" + url_encode(redirect_uri) +
        "&scope=" + scope +
        "&code_challenge=" + url_encode(code_challenge) +
        "&code_challenge_method=S256"
        "&state=" + url_encode(state) +
        "&id_token_add_organizations=true"
        "&codex_cli_simplified_flow=true"
        "&originator=" + url_encode(kOpenAIOriginator);
}

// ── Start OAuth flow (PKCE + authorize URL) ─────────────────────

OAuthFlowStart start_oauth_flow(const ProviderEntry& openai_entry) {
    std::string client_id = openai_entry.oauth_client_id.empty()
        ? kDefaultOAuthClientId
        : openai_entry.oauth_client_id;

    std::string state = generate_id();
    std::string verifier = make_code_verifier();
    std::string challenge = make_code_challenge_s256(verifier);

    OAuthFlowStart result;
    result.pending.provider = "openai";
    result.pending.state = state;
    result.pending.code_verifier = verifier;
    result.pending.redirect_uri = kDefaultRedirectUri;
    result.pending.created_at = epoch_seconds();
    result.authorize_url = build_authorize_url(
        client_id, kDefaultRedirectUri, challenge, state);
    return result;
}

// ── Token exchange ───────────────────────────────────────────────

std::string exchange_oauth_token(const std::string& code,
                                  const PendingOAuth& pending,
                                  const ProviderEntry& openai_entry,
                                  HttpClient& http,
                                  ProviderEntry& out_entry) {
    std::string token_url = openai_entry.oauth_token_url.empty()
        ? kDefaultTokenUrl
        : openai_entry.oauth_token_url;
    std::string client_id = openai_entry.oauth_client_id.empty()
        ? kDefaultOAuthClientId
        : openai_entry.oauth_client_id;

    // The exchange carries the authorization code and returns the refresh token, so the
    // endpoint has to be https before anything is sent to it.
    if (!is_https_url(token_url)) {
        return "OpenAI OAuth token endpoint must be https: " + token_url;
    }

    try {
        std::string body = form_encode({
            {"grant_type", "authorization_code"},
            {"code", code},
            {"redirect_uri", pending.redirect_uri},
            {"code_verifier", pending.code_verifier},
            {"client_id", client_id}
        });
        auto resp = http.post(token_url, body,
                              {{"Content-Type", "application/x-www-form-urlencoded"}},
                              kOAuthTokenTimeoutSeconds);

        if (resp.status_code < 200 || resp.status_code >= 300) {
            return "Token exchange failed (HTTP " +
                   std::to_string(resp.status_code) + ").";
        }
        if (resp.body.size() > kOAuthTokenBodyLimitBytes) {
            return "Token exchange response too large: " +
                   std::to_string(resp.body.size()) + " bytes.";
        }

        auto tok = json::parse(resp.body);
        std::string access = tok.value("access_token", "");
        if (access.empty()) {
            return "Token exchange succeeded but access_token is missing.";
        }
        std::string refresh = tok.value("refresh_token", "");
        uint64_t expires_in = tok.value("expires_in", 3600u);

        out_entry = openai_entry;
        out_entry.use_oauth = true;
        out_entry.oauth_access_token = access;
        if (!refresh.empty()) out_entry.oauth_refresh_token = refresh;
        out_entry.oauth_expires_at = epoch_seconds() + expires_in;
        if (out_entry.oauth_client_id.empty()) out_entry.oauth_client_id = client_id;
        if (out_entry.oauth_token_url.empty()) out_entry.oauth_token_url = token_url;
    } catch (const std::exception& e) {
        return std::string("OpenAI auth failed: ") + e.what();
    }

    return "";
}

// ── Config persistence ───────────────────────────────────────────

// ── Apply OAuth result (shared core) ─────────────────────────────

OAuthApplyResult apply_oauth_result(const std::string& code,
                                     const PendingOAuth& pending,
                                     Config& config,
                                     HttpClient& http) {
    OAuthApplyResult result;
    auto openai_it = config.providers.find("openai");
    if (openai_it == config.providers.end()) {
        result.error = "OpenAI provider config missing.";
        return result;
    }

    ProviderEntry updated;
    result.error = exchange_oauth_token(code, pending, openai_it->second, http, updated);
    if (!result.error.empty()) return result;

    config.providers["openai"] = updated;
    result.persisted = persist_openai_oauth(updated);
    result.success = true;
    return result;
}

// ── OAuth refresh callback wiring ─────────────────────────────────


} // namespace ptrclaw
