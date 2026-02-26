#include <catch2/catch_test_macros.hpp>
#include "onboard.hpp"

using namespace ptrclaw;

// ── needs_onboard ───────────────────────────────────────────────

TEST_CASE("needs_onboard: true when no credentials configured", "[onboard]") {
    Config cfg;
    cfg.provider = "anthropic";
    REQUIRE(needs_onboard(cfg));
}

TEST_CASE("needs_onboard: false when API key set for selected provider", "[onboard]") {
    Config cfg;
    cfg.provider = "anthropic";
    cfg.providers["anthropic"].api_key = "test-key";
    REQUIRE_FALSE(needs_onboard(cfg));
}

TEST_CASE("needs_onboard: false when OAuth token set for selected provider", "[onboard]") {
    Config cfg;
    cfg.provider = "openai";
    cfg.providers["openai"].oauth_access_token = "test-token";
    REQUIRE_FALSE(needs_onboard(cfg));
}

TEST_CASE("needs_onboard: false when Ollama is selected provider", "[onboard]") {
    Config cfg;
    cfg.provider = "ollama";
    // Ollama has no API key or OAuth — local provider, should not trigger
    REQUIRE_FALSE(needs_onboard(cfg));
}

TEST_CASE("needs_onboard: false when any provider has credentials", "[onboard]") {
    Config cfg;
    cfg.provider = "";
    cfg.providers["openai"].api_key = "sk-test";
    REQUIRE_FALSE(needs_onboard(cfg));
}

TEST_CASE("needs_onboard: true when provider empty and no credentials", "[onboard]") {
    Config cfg;
    cfg.provider = "";
    REQUIRE(needs_onboard(cfg));
}
