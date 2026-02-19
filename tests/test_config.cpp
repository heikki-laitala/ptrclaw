#include <catch2/catch_test_macros.hpp>
#include "config.hpp"
#include <fstream>
#include <filesystem>
#include <cstdlib>
#include <unistd.h>

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

// ── Config::load ────────────────────────────────────────────────

// Helper: create a temp directory
static std::string make_temp_dir() {
    auto path = std::filesystem::temp_directory_path() / "ptrclaw_cfg_XXXXXX";
    std::string tmpl = path.string();
    char* result = mkdtemp(tmpl.data());
    return result ? std::string(result) : "";
}

TEST_CASE("Config::load: reads config file", "[config]") {
    auto dir = make_temp_dir();
    REQUIRE_FALSE(dir.empty());

    // Create a config file
    std::string config_path = dir + "/config.json";
    {
        std::ofstream f(config_path);
        f << R"({
            "anthropic_api_key": "sk-file-ant",
            "openai_api_key": "sk-file-oai",
            "openrouter_api_key": "sk-file-or",
            "ollama_base_url": "http://custom:9999",
            "default_provider": "openai",
            "default_model": "gpt-4o",
            "default_temperature": 0.5,
            "agent": {
                "max_tool_iterations": 20,
                "max_history_messages": 100,
                "token_limit": 64000
            }
        })";
    }

    // Temporarily override HOME to redirect ~/.ptrclaw/config.json
    std::string old_home = std::getenv("HOME") ? std::getenv("HOME") : "";
    setenv("HOME", dir.c_str(), 1);
    std::filesystem::create_directories(dir + "/.ptrclaw");
    std::filesystem::copy_file(config_path, dir + "/.ptrclaw/config.json");

    // Clear env vars that would override
    unsetenv("ANTHROPIC_API_KEY");
    unsetenv("OPENAI_API_KEY");
    unsetenv("OPENROUTER_API_KEY");
    unsetenv("OLLAMA_BASE_URL");

    Config cfg = Config::load();

    REQUIRE(cfg.anthropic_api_key == "sk-file-ant");
    REQUIRE(cfg.openai_api_key == "sk-file-oai");
    REQUIRE(cfg.openrouter_api_key == "sk-file-or");
    REQUIRE(cfg.ollama_base_url == "http://custom:9999");
    REQUIRE(cfg.default_provider == "openai");
    REQUIRE(cfg.default_model == "gpt-4o");
    REQUIRE(cfg.default_temperature == 0.5);
    REQUIRE(cfg.agent.max_tool_iterations == 20);
    REQUIRE(cfg.agent.max_history_messages == 100);
    REQUIRE(cfg.agent.token_limit == 64000);

    setenv("HOME", old_home.c_str(), 1);
    std::filesystem::remove_all(dir);
}

TEST_CASE("Config::load: env vars override config file", "[config]") {
    auto dir = make_temp_dir();
    REQUIRE_FALSE(dir.empty());

    std::filesystem::create_directories(dir + "/.ptrclaw");
    {
        std::ofstream f(dir + "/.ptrclaw/config.json");
        f << R"({"anthropic_api_key": "from-file"})";
    }

    std::string old_home = std::getenv("HOME") ? std::getenv("HOME") : "";
    setenv("HOME", dir.c_str(), 1);
    setenv("ANTHROPIC_API_KEY", "from-env", 1);

    Config cfg = Config::load();
    REQUIRE(cfg.anthropic_api_key == "from-env");

    unsetenv("ANTHROPIC_API_KEY");
    setenv("HOME", old_home.c_str(), 1);
    std::filesystem::remove_all(dir);
}

TEST_CASE("Config::load: malformed JSON falls back to defaults", "[config]") {
    auto dir = make_temp_dir();
    REQUIRE_FALSE(dir.empty());

    std::filesystem::create_directories(dir + "/.ptrclaw");
    {
        std::ofstream f(dir + "/.ptrclaw/config.json");
        f << "not valid json {{{";
    }

    std::string old_home = std::getenv("HOME") ? std::getenv("HOME") : "";
    setenv("HOME", dir.c_str(), 1);
    unsetenv("ANTHROPIC_API_KEY");
    unsetenv("OPENAI_API_KEY");
    unsetenv("OPENROUTER_API_KEY");
    unsetenv("OLLAMA_BASE_URL");

    Config cfg = Config::load();
    REQUIRE(cfg.default_provider == "anthropic");
    REQUIRE(cfg.anthropic_api_key.empty());

    setenv("HOME", old_home.c_str(), 1);
    std::filesystem::remove_all(dir);
}

TEST_CASE("Config::load: missing config file uses defaults", "[config]") {
    auto dir = make_temp_dir();
    REQUIRE_FALSE(dir.empty());

    std::string old_home = std::getenv("HOME") ? std::getenv("HOME") : "";
    setenv("HOME", dir.c_str(), 1);
    unsetenv("ANTHROPIC_API_KEY");
    unsetenv("OPENAI_API_KEY");
    unsetenv("OPENROUTER_API_KEY");
    unsetenv("OLLAMA_BASE_URL");

    Config cfg = Config::load();
    REQUIRE(cfg.default_provider == "anthropic");
    REQUIRE(cfg.default_temperature == 0.7);

    setenv("HOME", old_home.c_str(), 1);
    std::filesystem::remove_all(dir);
}

TEST_CASE("Config::load: all env var overrides", "[config]") {
    auto dir = make_temp_dir();
    REQUIRE_FALSE(dir.empty());

    std::string old_home = std::getenv("HOME") ? std::getenv("HOME") : "";
    setenv("HOME", dir.c_str(), 1);
    setenv("ANTHROPIC_API_KEY", "env-ant", 1);
    setenv("OPENAI_API_KEY", "env-oai", 1);
    setenv("OPENROUTER_API_KEY", "env-or", 1);
    setenv("OLLAMA_BASE_URL", "http://env:1234", 1);

    Config cfg = Config::load();
    REQUIRE(cfg.anthropic_api_key == "env-ant");
    REQUIRE(cfg.openai_api_key == "env-oai");
    REQUIRE(cfg.openrouter_api_key == "env-or");
    REQUIRE(cfg.ollama_base_url == "http://env:1234");

    unsetenv("ANTHROPIC_API_KEY");
    unsetenv("OPENAI_API_KEY");
    unsetenv("OPENROUTER_API_KEY");
    unsetenv("OLLAMA_BASE_URL");
    setenv("HOME", old_home.c_str(), 1);
    std::filesystem::remove_all(dir);
}
