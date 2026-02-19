#include <catch2/catch_test_macros.hpp>
#include "mock_http_client.hpp"
#include "provider.hpp"
#include <stdexcept>

using namespace ptrclaw;

static MockHttpClient test_http;

// ── create_provider factory ─────────────────────────────────────

TEST_CASE("create_provider: anthropic returns valid provider", "[provider]") {
    auto p = create_provider("anthropic", "sk-test", test_http);
    REQUIRE(p != nullptr);
    REQUIRE(p->provider_name() == "anthropic");
    REQUIRE(p->supports_native_tools());
}

TEST_CASE("create_provider: openai returns valid provider", "[provider]") {
    auto p = create_provider("openai", "sk-test", test_http);
    REQUIRE(p != nullptr);
    REQUIRE(p->provider_name() == "openai");
    REQUIRE(p->supports_native_tools());
}

TEST_CASE("create_provider: ollama returns valid provider", "[provider]") {
    auto p = create_provider("ollama", "", test_http);
    REQUIRE(p != nullptr);
    REQUIRE(p->provider_name() == "ollama");
}

TEST_CASE("create_provider: ollama uses default URL", "[provider]") {
    auto p = create_provider("ollama", "", test_http, "");
    REQUIRE(p != nullptr);
}

TEST_CASE("create_provider: ollama uses custom URL", "[provider]") {
    auto p = create_provider("ollama", "", test_http, "http://custom:1234");
    REQUIRE(p != nullptr);
}

TEST_CASE("create_provider: openrouter returns valid provider", "[provider]") {
    auto p = create_provider("openrouter", "sk-test", test_http);
    REQUIRE(p != nullptr);
    REQUIRE(p->provider_name() == "openrouter");
}

TEST_CASE("create_provider: compatible returns valid provider", "[provider]") {
    auto p = create_provider("compatible", "sk-test", test_http, "http://localhost:8080");
    REQUIRE(p != nullptr);
    REQUIRE(p->provider_name() == "compatible");
}

TEST_CASE("create_provider: unknown provider throws", "[provider]") {
    REQUIRE_THROWS_AS(create_provider("unknown", "key", test_http), std::invalid_argument);
    REQUIRE_THROWS_AS(create_provider("", "key", test_http), std::invalid_argument);
}
