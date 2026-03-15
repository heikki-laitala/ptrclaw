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

// ── list_providers ──────────────────────────────────────────────

TEST_CASE("list_providers: empty config returns nothing", "[provider]") {
    Config cfg;
    auto result = list_providers(cfg, "anthropic");
    REQUIRE(result.empty());
}

TEST_CASE("list_providers: skips providers without credentials", "[provider]") {
    Config cfg;
    cfg.providers["anthropic"] = ProviderEntry{};  // no api_key
    auto result = list_providers(cfg, "");
    REQUIRE(result.empty());
}

TEST_CASE("list_providers: includes provider with api_key", "[provider]") {
    Config cfg;
    cfg.providers["anthropic"].api_key = "test-key";
    auto result = list_providers(cfg, "");
    REQUIRE(result.size() == 1);
    REQUIRE(result[0].name == "anthropic");
    REQUIRE(result[0].has_api_key);
    REQUIRE_FALSE(result[0].active);
}

TEST_CASE("list_providers: marks active provider", "[provider]") {
    Config cfg;
    cfg.providers["anthropic"].api_key = "test-key";
    auto result = list_providers(cfg, "anthropic");
    REQUIRE(result.size() == 1);
    REQUIRE(result[0].active);
}

TEST_CASE("list_providers: openai with api_key only", "[provider]") {
    Config cfg;
    cfg.providers["openai"].api_key = "sk-test";
    auto result = list_providers(cfg, "openai");
    REQUIRE(result.size() == 1);
    REQUIRE(result[0].name == "openai");
    REQUIRE(result[0].has_api_key);
    REQUIRE_FALSE(result[0].has_oauth);
    REQUIRE(result[0].active);
}

TEST_CASE("list_providers: openai with oauth only", "[provider]") {
    Config cfg;
    cfg.providers["openai"].oauth_access_token = "token";
    auto result = list_providers(cfg, "");
    REQUIRE(result.size() == 1);
    REQUIRE(result[0].has_oauth);
    REQUIRE_FALSE(result[0].has_api_key);
}

TEST_CASE("list_providers: openai with both api_key and oauth", "[provider]") {
    Config cfg;
    cfg.providers["openai"].api_key = "sk-test";
    cfg.providers["openai"].oauth_access_token = "token";
    auto result = list_providers(cfg, "");
    REQUIRE(result.size() == 1);
    REQUIRE(result[0].has_api_key);
    REQUIRE(result[0].has_oauth);
}

TEST_CASE("list_providers: openai without any credentials skipped", "[provider]") {
    Config cfg;
    cfg.providers["openai"] = ProviderEntry{};
    auto result = list_providers(cfg, "");
    REQUIRE(result.empty());
}

TEST_CASE("list_providers: local provider with base_url only when active", "[provider]") {
    Config cfg;
    cfg.providers["ollama"].base_url = "http://localhost:11434";
    // Not active — should not appear
    auto result1 = list_providers(cfg, "anthropic");
    REQUIRE(result1.empty());
    // Active — appears as local
    auto result2 = list_providers(cfg, "ollama");
    REQUIRE(result2.size() == 1);
    REQUIRE(result2[0].is_local);
    REQUIRE(result2[0].active);
}

TEST_CASE("list_providers: multiple providers", "[provider]") {
    Config cfg;
    cfg.providers["anthropic"].api_key = "key1";
    cfg.providers["openai"].api_key = "key2";
    auto result = list_providers(cfg, "anthropic");
    REQUIRE(result.size() == 2);
}

// ── auth_mode_label ─────────────────────────────────────────────

TEST_CASE("auth_mode_label: non-openai with api_key", "[provider]") {
    Config cfg;
    cfg.providers["anthropic"].api_key = "test-key";
    REQUIRE(auth_mode_label("anthropic", "claude-sonnet-4-6", cfg) == "API key");
}

TEST_CASE("auth_mode_label: non-openai without api_key returns local", "[provider]") {
    Config cfg;
    cfg.providers["ollama"] = ProviderEntry{};
    REQUIRE(auth_mode_label("ollama", "llama3", cfg) == "local");
}

TEST_CASE("auth_mode_label: openai non-codex returns API key", "[provider]") {
    Config cfg;
    cfg.providers["openai"].api_key = "sk-test";
    REQUIRE(auth_mode_label("openai", "gpt-4", cfg) == "API key");
}

TEST_CASE("auth_mode_label: openai codex with oauth returns OAuth", "[provider]") {
    Config cfg;
    cfg.providers["openai"].oauth_access_token = "token";
    REQUIRE(auth_mode_label("openai", "codex-mini", cfg) == "OAuth");
}

TEST_CASE("auth_mode_label: openai codex without oauth returns API key", "[provider]") {
    Config cfg;
    cfg.providers["openai"].api_key = "sk-test";
    REQUIRE(auth_mode_label("openai", "codex-mini", cfg) == "API key");
}

TEST_CASE("auth_mode_label: unknown provider returns API key", "[provider]") {
    Config cfg;
    // Provider not in config — falls through to "API key" default
    REQUIRE(auth_mode_label("unknown", "model", cfg) == "API key");
}

// ── switch_provider ─────────────────────────────────────────────

TEST_CASE("switch_provider: unknown provider returns error", "[provider]") {
    Config cfg;
    auto result = switch_provider("nonexistent", "", "model", cfg, test_http);
    REQUIRE_FALSE(result.error.empty());
    REQUIRE(result.error.find("Unknown provider") != std::string::npos);
    REQUIRE(result.provider == nullptr);
}

TEST_CASE("switch_provider: no credentials returns error", "[provider]") {
    Config cfg;
    cfg.providers["anthropic"] = ProviderEntry{};  // no key, no base_url
    auto result = switch_provider("anthropic", "", "model", cfg, test_http);
    REQUIRE_FALSE(result.error.empty());
    REQUIRE(result.error.find("No credentials") != std::string::npos);
}

TEST_CASE("switch_provider: openai no credentials returns error", "[provider]") {
    Config cfg;
    cfg.providers["openai"] = ProviderEntry{};
    auto result = switch_provider("openai", "", "gpt-4", cfg, test_http);
    REQUIRE_FALSE(result.error.empty());
    REQUIRE(result.error.find("No API key") != std::string::npos);
}

TEST_CASE("switch_provider: openai codex no credentials returns error", "[provider]") {
    Config cfg;
    cfg.providers["openai"] = ProviderEntry{};
    auto result = switch_provider("openai", "codex-mini", "gpt-4", cfg, test_http);
    REQUIRE_FALSE(result.error.empty());
    REQUIRE(result.error.find("OAuth") != std::string::npos);
}

TEST_CASE("switch_provider: valid provider with api_key succeeds", "[provider]") {
    REQUIRE_PROVIDER("anthropic");
    Config cfg;
    cfg.providers["anthropic"].api_key = "test-key";
    auto result = switch_provider("anthropic", "claude-sonnet", "old-model", cfg, test_http);
    REQUIRE(result.error.empty());
    REQUIRE(result.provider != nullptr);
    REQUIRE(result.model == "claude-sonnet");
}

TEST_CASE("switch_provider: openai with api_key succeeds", "[provider]") {
    REQUIRE_PROVIDER("openai");
    Config cfg;
    cfg.providers["openai"].api_key = "sk-test";
    auto result = switch_provider("openai", "gpt-4", "gpt-3.5", cfg, test_http);
    REQUIRE(result.error.empty());
    REQUIRE(result.provider != nullptr);
}
