#include <catch2/catch_test_macros.hpp>
#include "oauth.hpp"
#include "session.hpp"

using namespace ptrclaw;

// ── Constants ────────────────────────────────────────────────────

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

// ── oauth_url_encode ─────────────────────────────────────────────

TEST_CASE("oauth_url_encode: unreserved chars pass through", "[oauth]") {
    REQUIRE(oauth_url_encode("abc123") == "abc123");
    REQUIRE(oauth_url_encode("A-B_C.D~E") == "A-B_C.D~E");
}

TEST_CASE("oauth_url_encode: spaces encoded as %20", "[oauth]") {
    REQUIRE(oauth_url_encode("hello world") == "hello%20world");
}

TEST_CASE("oauth_url_encode: special chars encoded", "[oauth]") {
    auto encoded = oauth_url_encode("a=b&c");
    REQUIRE(encoded.find("%3D") != std::string::npos);
    REQUIRE(encoded.find("%26") != std::string::npos);
}

TEST_CASE("oauth_url_encode: empty string", "[oauth]") {
    REQUIRE(oauth_url_encode("").empty());
}

// ── form_encode ──────────────────────────────────────────────────

TEST_CASE("form_encode: builds key=value pairs", "[oauth]") {
    auto result = form_encode({{"grant_type", "authorization_code"}, {"code", "abc123"}});
    REQUIRE(result == "grant_type=authorization_code&code=abc123");
}

TEST_CASE("form_encode: encodes special characters in values", "[oauth]") {
    auto result = form_encode({{"redirect_uri", "http://localhost:1455/auth/callback"}});
    REQUIRE(result.find("http%3A%2F%2Flocalhost") != std::string::npos);
}

TEST_CASE("form_encode: empty params", "[oauth]") {
    REQUIRE(form_encode({}).empty());
}

TEST_CASE("form_encode: single param", "[oauth]") {
    auto result = form_encode({{"key", "value"}});
    REQUIRE(result == "key=value");
}

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

// ── build_authorize_url ──────────────────────────────────────────

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
    REQUIRE(url.find("originator=pi") != std::string::npos);
}

TEST_CASE("build_authorize_url: starts with authorize base URL", "[oauth]") {
    auto url = build_authorize_url("c", "r", "ch", "s");
    REQUIRE(url.rfind(kDefaultAuthorizeBaseUrl, 0) == 0);
}

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
