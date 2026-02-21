#include <catch2/catch_test_macros.hpp>
#include "mock_http_client.hpp"
#include "provider.hpp"
#include "plugin.hpp"
#include <stdexcept>

using namespace ptrclaw;

static MockHttpClient test_http;

// Helper: skip test if provider is not compiled in
#define REQUIRE_PROVIDER(name) \
    if (!PluginRegistry::instance().has_provider(name)) { SKIP(name " not compiled"); }

// ── create_provider factory ─────────────────────────────────────

TEST_CASE("create_provider: anthropic returns valid provider", "[provider]") {
    REQUIRE_PROVIDER("anthropic");
    auto p = create_provider("anthropic", "sk-test", test_http);
    REQUIRE(p != nullptr);
    REQUIRE(p->provider_name() == "anthropic");
    REQUIRE(p->supports_native_tools());
}

TEST_CASE("create_provider: openai returns valid provider", "[provider]") {
    REQUIRE_PROVIDER("openai");
    auto p = create_provider("openai", "sk-test", test_http);
    REQUIRE(p != nullptr);
    REQUIRE(p->provider_name() == "openai");
    REQUIRE(p->supports_native_tools());
}

TEST_CASE("create_provider: ollama returns valid provider", "[provider]") {
    REQUIRE_PROVIDER("ollama");
    auto p = create_provider("ollama", "", test_http);
    REQUIRE(p != nullptr);
    REQUIRE(p->provider_name() == "ollama");
}

TEST_CASE("create_provider: ollama uses default URL", "[provider]") {
    REQUIRE_PROVIDER("ollama");
    auto p = create_provider("ollama", "", test_http, "");
    REQUIRE(p != nullptr);
}

TEST_CASE("create_provider: ollama uses custom URL", "[provider]") {
    REQUIRE_PROVIDER("ollama");
    auto p = create_provider("ollama", "", test_http, "http://custom:1234");
    REQUIRE(p != nullptr);
}

TEST_CASE("create_provider: openrouter returns valid provider", "[provider]") {
    REQUIRE_PROVIDER("openrouter");
    auto p = create_provider("openrouter", "sk-test", test_http);
    REQUIRE(p != nullptr);
    REQUIRE(p->provider_name() == "openrouter");
}

TEST_CASE("create_provider: compatible returns valid provider", "[provider]") {
    REQUIRE_PROVIDER("compatible");
    auto p = create_provider("compatible", "sk-test", test_http, "http://localhost:8080");
    REQUIRE(p != nullptr);
    REQUIRE(p->provider_name() == "compatible");
}

TEST_CASE("create_provider: unknown provider throws", "[provider]") {
    REQUIRE_THROWS_AS(create_provider("unknown", "key", test_http), std::invalid_argument);
    REQUIRE_THROWS_AS(create_provider("", "key", test_http), std::invalid_argument);
}
