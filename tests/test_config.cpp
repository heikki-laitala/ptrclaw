#include <catch2/catch_test_macros.hpp>
#include "config.hpp"
#include <fstream>
#include <filesystem>
#include <cstdlib>
#include <unistd.h>
#include <nlohmann/json.hpp>

using namespace ptrclaw;

// ── Default values ───────────────────────────────────────────────

TEST_CASE("Config: default values are sensible", "[config]") {
    Config cfg;
    REQUIRE(cfg.provider == "anthropic");
    REQUIRE(cfg.temperature == 0.7);
    REQUIRE(cfg.api_key_for("anthropic").empty());
    REQUIRE(cfg.api_key_for("openai").empty());
    REQUIRE(cfg.api_key_for("openrouter").empty());
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
    cfg.providers["anthropic"].api_key = "sk-ant-123";
    cfg.providers["openai"].api_key = "sk-oai-456";
    cfg.providers["openrouter"].api_key = "sk-or-789";

    REQUIRE(cfg.api_key_for("anthropic") == "sk-ant-123");
    REQUIRE(cfg.api_key_for("openai") == "sk-oai-456");
    REQUIRE(cfg.api_key_for("openrouter") == "sk-or-789");
}

TEST_CASE("Config::api_key_for: unknown provider returns empty", "[config]") {
    Config cfg;
    cfg.providers["anthropic"].api_key = "key";
    REQUIRE(cfg.api_key_for("unknown").empty());
    REQUIRE(cfg.api_key_for("").empty());
}

// ── base_url_for ─────────────────────────────────────────────────

TEST_CASE("Config::base_url_for: returns correct URL per provider", "[config]") {
    Config cfg;
    cfg.providers["ollama"].base_url = "http://ollama:11434";
    cfg.providers["compatible"].base_url = "http://local:8080/v1";

    REQUIRE(cfg.base_url_for("ollama") == "http://ollama:11434");
    REQUIRE(cfg.base_url_for("compatible") == "http://local:8080/v1");
}

TEST_CASE("Config::base_url_for: other providers return empty", "[config]") {
    Config cfg;
    cfg.providers["ollama"].base_url = "http://ollama:11434";
    REQUIRE(cfg.base_url_for("anthropic").empty());
    REQUIRE(cfg.base_url_for("openai").empty());
    REQUIRE(cfg.base_url_for("openrouter").empty());
    REQUIRE(cfg.base_url_for("unknown").empty());
}

// ── Config::load ────────────────────────────────────────────────

// Helper: create a temp directory
static std::string make_temp_dir() {
    auto path = std::filesystem::temp_directory_path() / "ptrclaw_cfg_XXXXXX";
    std::string tmpl = path.string();
    char* result = mkdtemp(tmpl.data());
    return result ? std::string(result) : "";
}

// RAII guard: redirects HOME to a temp dir, clears env vars, restores on destruction
struct ConfigTestGuard {
    std::string dir;
    std::string old_home;

    ConfigTestGuard() {
        dir = make_temp_dir();
        old_home = std::getenv("HOME") ? std::getenv("HOME") : "";
        setenv("HOME", dir.c_str(), 1);
        unsetenv("ANTHROPIC_API_KEY");
        unsetenv("OPENAI_API_KEY");
        unsetenv("OPENROUTER_API_KEY");
        unsetenv("OLLAMA_BASE_URL");
    }

    ~ConfigTestGuard() {
        setenv("HOME", old_home.c_str(), 1);
        std::filesystem::remove_all(dir);
    }

    ConfigTestGuard(const ConfigTestGuard&) = delete;
    ConfigTestGuard& operator=(const ConfigTestGuard&) = delete;

    std::string config_path() const { return dir + "/.ptrclaw/config.json"; }

    void write_config(const std::string& content) {
        std::filesystem::create_directories(dir + "/.ptrclaw");
        std::ofstream f(config_path());
        f << content;
    }
};

TEST_CASE("Config::load: reads config file", "[config]") {
    ConfigTestGuard g;
    REQUIRE_FALSE(g.dir.empty());

    g.write_config(R"({
        "providers": {
            "anthropic": { "api_key": "sk-file-ant" },
            "openai": { "api_key": "sk-file-oai" },
            "openrouter": { "api_key": "sk-file-or" },
            "ollama": { "base_url": "http://custom:9999" }
        },
        "provider": "openai",
        "model": "gpt-4o",
        "temperature": 0.5,
        "agent": {
            "max_tool_iterations": 20,
            "max_history_messages": 100,
            "token_limit": 64000
        }
    })");

    Config cfg = Config::load();

    REQUIRE(cfg.api_key_for("anthropic") == "sk-file-ant");
    REQUIRE(cfg.api_key_for("openai") == "sk-file-oai");
    REQUIRE(cfg.api_key_for("openrouter") == "sk-file-or");
    REQUIRE(cfg.base_url_for("ollama") == "http://custom:9999");
    REQUIRE(cfg.provider == "openai");
    REQUIRE(cfg.model == "gpt-4o");
    REQUIRE(cfg.temperature == 0.5);
    REQUIRE(cfg.agent.max_tool_iterations == 20);
    REQUIRE(cfg.agent.max_history_messages == 100);
    REQUIRE(cfg.agent.token_limit == 64000);
}

TEST_CASE("Config::load: env vars override config file", "[config]") {
    ConfigTestGuard g;
    REQUIRE_FALSE(g.dir.empty());

    g.write_config(R"({"providers": {"anthropic": {"api_key": "from-file"}}})");
    setenv("ANTHROPIC_API_KEY", "from-env", 1);

    Config cfg = Config::load();
    REQUIRE(cfg.api_key_for("anthropic") == "from-env");

    unsetenv("ANTHROPIC_API_KEY");
}

TEST_CASE("Config::load: malformed JSON falls back to defaults", "[config]") {
    ConfigTestGuard g;
    REQUIRE_FALSE(g.dir.empty());

    g.write_config("not valid json {{{");

    Config cfg = Config::load();
    REQUIRE(cfg.provider == "anthropic");
    REQUIRE(cfg.api_key_for("anthropic").empty());
}

TEST_CASE("Config::load: missing config file uses defaults", "[config]") {
    ConfigTestGuard g;
    REQUIRE_FALSE(g.dir.empty());

    Config cfg = Config::load();
    REQUIRE(cfg.provider == "anthropic");
    REQUIRE(cfg.temperature == 0.7);
}

TEST_CASE("Config::load: all env var overrides", "[config]") {
    ConfigTestGuard g;
    REQUIRE_FALSE(g.dir.empty());

    setenv("ANTHROPIC_API_KEY", "env-ant", 1);
    setenv("OPENAI_API_KEY", "env-oai", 1);
    setenv("OPENROUTER_API_KEY", "env-or", 1);
    setenv("OLLAMA_BASE_URL", "http://env:1234", 1);

    Config cfg = Config::load();
    REQUIRE(cfg.api_key_for("anthropic") == "env-ant");
    REQUIRE(cfg.api_key_for("openai") == "env-oai");
    REQUIRE(cfg.api_key_for("openrouter") == "env-or");
    REQUIRE(cfg.base_url_for("ollama") == "http://env:1234");

    unsetenv("ANTHROPIC_API_KEY");
    unsetenv("OPENAI_API_KEY");
    unsetenv("OPENROUTER_API_KEY");
    unsetenv("OLLAMA_BASE_URL");
}

// ── Default config creation and migration ────────────────────────

TEST_CASE("Config::load: creates default config when missing", "[config]") {
    ConfigTestGuard g;
    REQUIRE_FALSE(g.dir.empty());

    Config::load();

    REQUIRE(std::filesystem::exists(g.config_path()));

    std::ifstream f(g.config_path());
    nlohmann::json j = nlohmann::json::parse(f);

    REQUIRE(j.contains("provider"));
    REQUIRE(j["provider"] == "anthropic");
    REQUIRE(j.contains("providers"));
    REQUIRE(j["providers"].contains("anthropic"));
    REQUIRE(j.contains("agent"));
    REQUIRE(j["agent"].contains("max_tool_iterations"));
    REQUIRE(j.contains("memory"));
    REQUIRE(j["memory"].contains("backend"));
#ifdef PTRCLAW_HAS_SQLITE_MEMORY
    REQUIRE(j["memory"]["backend"] == "sqlite");
#else
    REQUIRE(j["memory"]["backend"] == "json");
#endif

    // Channels section present with empty defaults
    REQUIRE(j.contains("channels"));
    REQUIRE(j["channels"].contains("telegram"));
    REQUIRE(j["channels"]["telegram"]["bot_token"] == "");
    REQUIRE(j["channels"].contains("whatsapp"));
    REQUIRE(j["channels"]["whatsapp"]["access_token"] == "");
}

TEST_CASE("Config::load: migrates existing config with missing keys", "[config]") {
    ConfigTestGuard g;
    REQUIRE_FALSE(g.dir.empty());

    g.write_config(R"({"providers": {"anthropic": {"api_key": "sk-test"}}, "model": "gpt-4o"})");

    Config cfg = Config::load();

    // User values preserved
    REQUIRE(cfg.api_key_for("anthropic") == "sk-test");
    REQUIRE(cfg.model == "gpt-4o");

    // Re-read file to verify migration wrote new keys
    std::ifstream f(g.config_path());
    nlohmann::json j = nlohmann::json::parse(f);

    REQUIRE(j["providers"]["anthropic"]["api_key"] == "sk-test");
    REQUIRE(j["model"] == "gpt-4o");
    REQUIRE(j.contains("memory"));
#ifdef PTRCLAW_HAS_SQLITE_MEMORY
    REQUIRE(j["memory"]["backend"] == "sqlite");
#else
    REQUIRE(j["memory"]["backend"] == "json");
#endif
    REQUIRE(j.contains("agent"));
    REQUIRE(j["agent"]["max_tool_iterations"] == 10);
}

TEST_CASE("Config::load: does not rewrite complete config", "[config]") {
    ConfigTestGuard g;
    REQUIRE_FALSE(g.dir.empty());

    // Start from actual defaults, override a few values to prove they survive
    nlohmann::json full = Config::defaults_json();
    full["provider"] = "openai";
    full["model"] = "gpt-4o";
    full["agent"]["max_tool_iterations"] = 5;

    g.write_config(full.dump(4) + "\n");

    // Record content before loading
    std::string before;
    {
        std::ifstream f(g.config_path());
        before.assign(std::istreambuf_iterator<char>(f),
                      std::istreambuf_iterator<char>());
    }

    Config cfg = Config::load();

    // Custom values preserved
    REQUIRE(cfg.provider == "openai");
    REQUIRE(cfg.model == "gpt-4o");
    REQUIRE(cfg.agent.max_tool_iterations == 5);

    // File should be unchanged — no unnecessary rewrite
    std::string after;
    {
        std::ifstream f(g.config_path());
        after.assign(std::istreambuf_iterator<char>(f),
                     std::istreambuf_iterator<char>());
    }
    REQUIRE(before == after);
}

TEST_CASE("Config::load: defaults roundtrip without re-migration", "[config]") {
    ConfigTestGuard g;
    REQUIRE_FALSE(g.dir.empty());

    // First load creates the default config file
    Config::load();

    // Read the written file
    std::string first;
    {
        std::ifstream f(g.config_path());
        first.assign(std::istreambuf_iterator<char>(f),
                     std::istreambuf_iterator<char>());
    }

    // Second load should find nothing to migrate
    Config::load();

    std::string second;
    {
        std::ifstream f(g.config_path());
        second.assign(std::istreambuf_iterator<char>(f),
                      std::istreambuf_iterator<char>());
    }
    REQUIRE(first == second);
}
