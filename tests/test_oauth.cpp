#include <catch2/catch_test_macros.hpp>
#include "oauth.hpp"
#include "session.hpp"
#include "mock_http_client.hpp"
#ifdef PTRCLAW_HAS_OPENAI_OAUTH
#include "providers/oauth_openai.hpp"
#include "providers/openai.hpp"
#endif

using namespace ptrclaw;

// ── Constants (OpenAI-specific) ──────────────────────────────────

#ifdef PTRCLAW_HAS_OPENAI_OAUTH
TEST_CASE("OAuth: default client_id matches Codex CLI", "[oauth]") {
    REQUIRE(std::string(kDefaultOAuthClientId) == "app_EMoamEEZ73f0CkXaXp7hrann");
}

TEST_CASE("OAuth: default redirect_uri uses localhost", "[oauth]") {
    std::string uri(kDefaultRedirectUri);
    REQUIRE(uri.find("localhost") != std::string::npos);
    REQUIRE(uri == "http://localhost:1455/auth/callback");
}

TEST_CASE("OAuth: default token URL", "[oauth]") {
    REQUIRE(std::string(kDefaultTokenUrl) == "https://auth.openai.com/oauth/token");
}

TEST_CASE("OAuth: default authorize base URL", "[oauth]") {
    REQUIRE(std::string(kDefaultAuthorizeBaseUrl) == "https://auth.openai.com/oauth/authorize");
}

// ── Model kept after connecting ─────────────────────────────────

TEST_CASE("OAuth: default OAuth model is a subscription model", "[oauth]") {
    REQUIRE(std::string(kDefaultOAuthModel) == "gpt-5.6-sol");
}

TEST_CASE("OAuth: connecting keeps a model the subscription serves", "[oauth]") {
    ProviderEntry entry;
    REQUIRE(oauth_model_after_connect("openai", "gpt-5.5", entry) == "gpt-5.5");
    REQUIRE(oauth_model_after_connect("openai", "gpt-5.3-codex", entry) == "gpt-5.3-codex");
}

TEST_CASE("OAuth: connecting moves off a model the subscription cannot serve", "[oauth]") {
    ProviderEntry entry;
    REQUIRE(oauth_model_after_connect("openai", "gpt-4o", entry) ==
            std::string(kDefaultOAuthModel));
    REQUIRE(oauth_model_after_connect("openai", "gpt-5.6", entry) ==
            std::string(kDefaultOAuthModel));
    REQUIRE(oauth_model_after_connect("openai", "", entry) ==
            std::string(kDefaultOAuthModel));
}

TEST_CASE("OAuth: connecting honours oauth_models when keeping the model", "[oauth]") {
    ProviderEntry entry;
    entry.oauth_models = {"gpt-4o"};
    REQUIRE(oauth_model_after_connect("openai", "gpt-4o", entry) == "gpt-4o");
    REQUIRE(oauth_model_after_connect("openai", "gpt-5.5", entry) ==
            std::string(kDefaultOAuthModel));
}

// Connecting OpenAI OAuth while on another provider's model must not point that model at
// the ChatGPT backend, however permissive oauth_models is.
TEST_CASE("OAuth: connecting from another provider always takes the default", "[oauth]") {
    ProviderEntry entry;
    REQUIRE(oauth_model_after_connect("anthropic", "claude-sonnet-4-6", entry) ==
            std::string(kDefaultOAuthModel));

    entry.oauth_models = {"*"};
    REQUIRE(oauth_model_after_connect("anthropic", "claude-sonnet-4-6", entry) ==
            std::string(kDefaultOAuthModel));
    REQUIRE(oauth_model_after_connect("ollama", "llama3.2", entry) ==
            std::string(kDefaultOAuthModel));
}

// ── Token endpoint guards ───────────────────────────────────────

namespace {

PendingOAuth test_pending() {
    PendingOAuth pending;
    pending.provider = "openai";
    pending.state = "state";
    pending.code_verifier = "verifier";
    pending.redirect_uri = kDefaultRedirectUri;
    return pending;
}

} // namespace

// The authorization code exchange carries the code and returns the refresh token, so the
// same plaintext rule applies as on refresh.
TEST_CASE("OAuth: token exchange refuses a plaintext token endpoint", "[oauth]") {
    MockHttpClient mock;
    ProviderEntry entry;
    entry.oauth_token_url = "http://auth.test/token";
    ProviderEntry out;

    auto error = exchange_oauth_token("code", test_pending(), entry, mock, out);
    REQUIRE_FALSE(error.empty());
    REQUIRE(mock.call_count == 0);
}

TEST_CASE("OAuth: token exchange rejects an oversized response", "[oauth]") {
    MockHttpClient mock;
    mock.next_response = {200, std::string(kOAuthTokenBodyLimitBytes + 1, 'x')};
    ProviderEntry entry;
    entry.oauth_token_url = "https://auth.test/token";
    ProviderEntry out;

    auto error = exchange_oauth_token("code", test_pending(), entry, mock, out);
    REQUIRE_FALSE(error.empty());
    REQUIRE(out.oauth_access_token.empty());
}

TEST_CASE("OAuth: token exchange does not wait out the chat timeout", "[oauth]") {
    MockHttpClient mock;
    mock.next_response = {200, R"({
        "access_token": "access",
        "refresh_token": "refresh",
        "expires_in": 3600
    })"};
    ProviderEntry entry;
    entry.oauth_token_url = "https://auth.test/token";
    ProviderEntry out;

    auto error = exchange_oauth_token("code", test_pending(), entry, mock, out);
    REQUIRE(error.empty());
    REQUIRE(out.oauth_access_token == "access");
    REQUIRE(mock.last_timeout == 30);
}
#endif

// url_encode/form_encode tests moved to test_util.cpp along with the functions —
// they are built in every configuration, so their tests must be too.

// ── make_code_verifier ───────────────────────────────────────────

TEST_CASE("make_code_verifier: non-empty", "[oauth]") {
    auto v = make_code_verifier();
    REQUIRE_FALSE(v.empty());
}

TEST_CASE("make_code_verifier: reasonable length", "[oauth]") {
    auto v = make_code_verifier();
    REQUIRE(v.size() >= 32);
    REQUIRE(v.size() <= 128);
}

TEST_CASE("make_code_verifier: contains only base64url chars", "[oauth]") {
    auto v = make_code_verifier();
    for (char c : v) {
        bool valid = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                     (c >= '0' && c <= '9') || c == '-' || c == '_';
        REQUIRE(valid);
    }
}

// ── make_code_challenge_s256 ─────────────────────────────────────

TEST_CASE("make_code_challenge_s256: non-empty", "[oauth]") {
    auto c = make_code_challenge_s256("test-verifier");
    REQUIRE_FALSE(c.empty());
}

TEST_CASE("make_code_challenge_s256: deterministic", "[oauth]") {
    auto c1 = make_code_challenge_s256("same-input");
    auto c2 = make_code_challenge_s256("same-input");
    REQUIRE(c1 == c2);
}

TEST_CASE("make_code_challenge_s256: different input gives different output", "[oauth]") {
    auto c1 = make_code_challenge_s256("input-a");
    auto c2 = make_code_challenge_s256("input-b");
    REQUIRE(c1 != c2);
}

// ── build_authorize_url (OpenAI-specific) ────────────────────────

#ifdef PTRCLAW_HAS_OPENAI_OAUTH
TEST_CASE("build_authorize_url: contains all required params", "[oauth]") {
    auto url = build_authorize_url("test-client", "http://localhost:1455/auth/callback",
                                    "test-challenge", "test-state");
    REQUIRE(url.find("response_type=code") != std::string::npos);
    REQUIRE(url.find("client_id=test-client") != std::string::npos);
    REQUIRE(url.find("redirect_uri=") != std::string::npos);
    REQUIRE(url.find("scope=openid+profile+email+offline_access") != std::string::npos);
    REQUIRE(url.find("code_challenge=test-challenge") != std::string::npos);
    REQUIRE(url.find("code_challenge_method=S256") != std::string::npos);
    REQUIRE(url.find("state=test-state") != std::string::npos);
    REQUIRE(url.find("id_token_add_organizations=true") != std::string::npos);
    REQUIRE(url.find("codex_cli_simplified_flow=true") != std::string::npos);
    // The value paired with the built-in client_id; see kOpenAIOriginator.
    REQUIRE(url.find("originator=pi") != std::string::npos);
}

TEST_CASE("build_authorize_url: starts with authorize base URL", "[oauth]") {
    auto url = build_authorize_url("c", "r", "ch", "s");
    REQUIRE(url.rfind(kDefaultAuthorizeBaseUrl, 0) == 0);
}
#endif

// ── parse_oauth_input ────────────────────────────────────────────

TEST_CASE("parse_oauth_input: bare code", "[oauth]") {
    auto r = parse_oauth_input("abc123def");
    REQUIRE(r.code == "abc123def");
    REQUIRE(r.state.empty());
}

TEST_CASE("parse_oauth_input: full callback URL", "[oauth]") {
    auto r = parse_oauth_input("http://localhost:1455/auth/callback?code=mycode&state=mystate");
    REQUIRE(r.code == "mycode");
    REQUIRE(r.state == "mystate");
}

TEST_CASE("parse_oauth_input: URL with code only", "[oauth]") {
    auto r = parse_oauth_input("http://localhost:1455/auth/callback?code=justcode");
    REQUIRE(r.code == "justcode");
    REQUIRE(r.state.empty());
}

TEST_CASE("parse_oauth_input: trims whitespace", "[oauth]") {
    auto r = parse_oauth_input("  abc123  ");
    REQUIRE(r.code == "abc123");
}

TEST_CASE("parse_oauth_input: URL with fragment", "[oauth]") {
    auto r = parse_oauth_input("http://localhost:1455/auth/callback?code=c1&state=s1#extra");
    REQUIRE(r.code == "c1");
    REQUIRE(r.state == "s1");
}
