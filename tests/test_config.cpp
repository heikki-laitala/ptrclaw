#include <catch2/catch_test_macros.hpp>
#include "config.hpp"

using namespace ptrclaw;

// ── Default values ───────────────────────────────────────────────

TEST_CASE("Config: default values are sensible", "[config]") {
    Config cfg;
    REQUIRE(cfg.default_provider == "anthropic");
    REQUIRE(cfg.default_temperature == 0.7);
    REQUIRE(cfg.ollama_base_url == "http://localhost:11434");
    REQUIRE(cfg.anthropic_api_key.empty());
    REQUIRE(cfg.openai_api_key.empty());
    REQUIRE(cfg.openrouter_api_key.empty());
}

TEST_CASE("AgentConfig: default values", "[config]") {
    AgentConfig ac;
    REQUIRE(ac.max_tool_iterations == 10);
    REQUIRE(ac.max_history_messages == 50);
    REQUIRE(ac.token_limit == 128000);
}

// ── api_key_for ──────────────────────────────────────────────────

TEST_CASE("Config::api_key_for: returns correct key per provider", "[config]") {
    Config cfg;
    cfg.anthropic_api_key = "sk-ant-123";
    cfg.openai_api_key = "sk-oai-456";
    cfg.openrouter_api_key = "sk-or-789";

    REQUIRE(cfg.api_key_for("anthropic") == "sk-ant-123");
    REQUIRE(cfg.api_key_for("openai") == "sk-oai-456");
    REQUIRE(cfg.api_key_for("openrouter") == "sk-or-789");
}

TEST_CASE("Config::api_key_for: unknown provider returns empty", "[config]") {
    Config cfg;
    cfg.anthropic_api_key = "key";
    REQUIRE(cfg.api_key_for("unknown").empty());
    REQUIRE(cfg.api_key_for("ollama").empty());
    REQUIRE(cfg.api_key_for("").empty());
}
