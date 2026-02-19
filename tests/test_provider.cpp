#include <catch2/catch_test_macros.hpp>
#include "provider.hpp"
#include <stdexcept>

using namespace ptrclaw;

// ── create_provider factory ─────────────────────────────────────

TEST_CASE("create_provider: anthropic returns valid provider", "[provider]") {
    auto p = create_provider("anthropic", "sk-test");
    REQUIRE(p != nullptr);
    REQUIRE(p->provider_name() == "anthropic");
    REQUIRE(p->supports_native_tools());
}

TEST_CASE("create_provider: openai returns valid provider", "[provider]") {
    auto p = create_provider("openai", "sk-test");
    REQUIRE(p != nullptr);
    REQUIRE(p->provider_name() == "openai");
    REQUIRE(p->supports_native_tools());
}

TEST_CASE("create_provider: ollama returns valid provider", "[provider]") {
    auto p = create_provider("ollama", "");
    REQUIRE(p != nullptr);
    REQUIRE(p->provider_name() == "ollama");
}

TEST_CASE("create_provider: ollama uses default URL", "[provider]") {
    auto p = create_provider("ollama", "", "");
    REQUIRE(p != nullptr);
}

TEST_CASE("create_provider: ollama uses custom URL", "[provider]") {
    auto p = create_provider("ollama", "", "http://custom:1234");
    REQUIRE(p != nullptr);
}

TEST_CASE("create_provider: openrouter returns valid provider", "[provider]") {
    auto p = create_provider("openrouter", "sk-test");
    REQUIRE(p != nullptr);
    REQUIRE(p->provider_name() == "openrouter");
}

TEST_CASE("create_provider: compatible returns valid provider", "[provider]") {
    auto p = create_provider("compatible", "sk-test", "http://localhost:8080");
    REQUIRE(p != nullptr);
    REQUIRE(p->provider_name() == "compatible");
}

TEST_CASE("create_provider: unknown provider throws", "[provider]") {
    REQUIRE_THROWS_AS(create_provider("unknown", "key"), std::invalid_argument);
    REQUIRE_THROWS_AS(create_provider("", "key"), std::invalid_argument);
}
