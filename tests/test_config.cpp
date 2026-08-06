#include <catch2/catch_test_macros.hpp>
#include "config.hpp"
#include "test_helpers.hpp"
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
    REQUIRE(ac.max_tool_iterations == 50);
    REQUIRE(ac.max_history_messages == 50);
    REQUIRE(ac.token_limit == 128000);
    // The hour that used to be hard-coded at the eviction call site. Pinned so making it
    // configurable cannot quietly change what an unconfigured deployment does.
    REQUIRE(ac.session_max_idle_seconds == 3600);
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
        // One getenv call, not two: the second could return null where the first
        // did not, and assigning null to std::string is undefined behaviour.
        const char* home = std::getenv("HOME");
        old_home = home ? home : "";
        setenv("HOME", dir.c_str(), 1);
        unsetenv("ANTHROPIC_API_KEY");
        unsetenv("OPENAI_API_KEY");
        unsetenv("OPENROUTER_API_KEY");
        unsetenv("OLLAMA_BASE_URL");
    }

    ~ConfigTestGuard() noexcept {
        setenv("HOME", old_home.c_str(), 1);
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
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

// ── serving profile ─────────────────────────────────────────────

TEST_CASE("ServingConfig: defaults are inert", "[config][serving]") {
    Config cfg;
    // Absent keys must leave the personal-agent behaviour untouched: no roots means no
    // scoping, and ids stay mandatory the way HttpChannel has always required.
    REQUIRE(cfg.serving.context_dir.empty());
    REQUIRE(cfg.serving.workspace_root.empty());
    REQUIRE_FALSE(cfg.serving.generate_session_ids);
}

TEST_CASE("Config::load: reads the serving section", "[config][serving]") {
    ConfigTestGuard g;
    REQUIRE_FALSE(g.dir.empty());
    g.write_config(R"({
        "serving": {
            "context_dir": "/work/context",
            "workspace_root": "/work/sessions",
            "generate_session_ids": true
        }
    })");

    Config cfg = Config::load();
    REQUIRE(cfg.serving.context_dir == "/work/context");
    REQUIRE(cfg.serving.workspace_root == "/work/sessions");
    REQUIRE(cfg.serving.generate_session_ids);
}

TEST_CASE("Config::load: serving paths expand ~", "[config][serving]") {
    ConfigTestGuard g;
    REQUIRE_FALSE(g.dir.empty());
    g.write_config(R"({"serving": {"workspace_root": "~/work"}})");

    Config cfg = Config::load();
    // Stored expanded, like memory.path is resolved before use — a tool comparing
    // canonical prefixes cannot do anything with a literal "~".
    REQUIRE(cfg.serving.workspace_root.rfind('~') == std::string::npos);
    REQUIRE(cfg.serving.workspace_root.find("/work") != std::string::npos);
}

TEST_CASE("Config::load: wrong serving types keep the defaults", "[config][serving]") {
    ConfigTestGuard g;
    REQUIRE_FALSE(g.dir.empty());
    g.write_config(R"({
        "serving": {
            "context_dir": 42,
            "workspace_root": null,
            "generate_session_ids": "yes"
        }
    })");

    Config cfg = Config::load();
    REQUIRE(cfg.serving.context_dir.empty());
    REQUIRE(cfg.serving.workspace_root.empty());
    REQUIRE_FALSE(cfg.serving.generate_session_ids);
}

// The generated default config is what an existing personal install gets migrated to, so
// the serving keys must stay out of it.
TEST_CASE("Config::defaults_json: carries no serving section", "[config][serving]") {
    REQUIRE_FALSE(Config::defaults_json().contains("serving"));
}

TEST_CASE("Config::load: reads openai oauth_models", "[config][oauth]") {
    ConfigTestGuard g;
    REQUIRE_FALSE(g.dir.empty());
    g.write_config(R"({
        "providers": {
            "openai": { "oauth_models": ["gpt-5", "gpt-4o"] }
        }
    })");

    Config cfg = Config::load();
    REQUIRE(cfg.providers["openai"].oauth_models ==
            std::vector<std::string>{"gpt-5", "gpt-4o"});
}

TEST_CASE("Config::load: oauth_models ignores non-string entries", "[config][oauth]") {
    ConfigTestGuard g;
    REQUIRE_FALSE(g.dir.empty());
    g.write_config(R"({
        "providers": {
            "openai": { "oauth_models": ["gpt-5", 42, null] }
        }
    })");

    Config cfg = Config::load();
    REQUIRE(cfg.providers["openai"].oauth_models ==
            std::vector<std::string>{"gpt-5"});
}

TEST_CASE("Config::load: OPENAI_OAUTH_MODELS overrides the config file", "[config][oauth]") {
    ConfigTestGuard g;
    REQUIRE_FALSE(g.dir.empty());
    g.write_config(R"({"providers": {"openai": {"oauth_models": ["gpt-5"]}}})");
    setenv("OPENAI_OAUTH_MODELS", "o3, gpt-4o ", 1);

    Config cfg = Config::load();
    unsetenv("OPENAI_OAUTH_MODELS");

    REQUIRE(cfg.providers["openai"].oauth_models ==
            std::vector<std::string>{"o3", "gpt-4o"});
}

TEST_CASE("AgentConfig: session_max_idle_seconds is read from the config file",
          "[config]") {
    ConfigTestGuard g;
    REQUIRE_FALSE(g.dir.empty());
    g.write_config(R"({"agent": {"session_max_idle_seconds": 90}})");
    REQUIRE(Config::load().agent.session_max_idle_seconds == 90);
}

TEST_CASE("AgentConfig: a non-numeric session_max_idle_seconds keeps the default",
          "[config]") {
    // Every other field in this block ignores a wrongly typed value rather than failing
    // the load; this one does the same, and the default it falls back to is the safe
    // direction — sessions live longer than intended, not shorter.
    ConfigTestGuard g;
    REQUIRE_FALSE(g.dir.empty());
    g.write_config(R"({"agent": {"session_max_idle_seconds": "not-a-number"}})");
    REQUIRE(Config::load().agent.session_max_idle_seconds == 3600);
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
    REQUIRE(j["channels"]["telegram"]["bot_token"].get<std::string>().empty());
    REQUIRE(j["channels"].contains("whatsapp"));
    REQUIRE(j["channels"]["whatsapp"]["access_token"].get<std::string>().empty());
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
    REQUIRE(j["agent"]["max_tool_iterations"] == 50);
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

// ── Channel command gating ──────────────────────────────────────

TEST_CASE("Config: channel commands are off unless asked for", "[config]") {
    // The default is the security property. An operator who never reads this option
    // gets an agent whose command surface is unreachable from a channel, and so does
    // every channel added later.
    Config cfg;
    REQUIRE_FALSE(cfg.allow_channel_commands);
    REQUIRE_FALSE(Config::defaults_json()["allow_channel_commands"].get<bool>());
}

TEST_CASE("Config: allow_channel_commands is read from the file", "[config]") {
    HomeGuard home;
    home.write_default_config();
    modify_config_json([](nlohmann::json& j) { j["allow_channel_commands"] = true; });
    REQUIRE(Config::load().allow_channel_commands);
}

TEST_CASE("Config: a non-boolean allow_channel_commands leaves the safe default",
          "[config]") {
    // Fail closed on malformed config rather than coercing a truthy value: "false" as a
    // string must not read as true, which is how this kind of flag gets silently flipped.
    HomeGuard home;
    home.write_default_config();
    modify_config_json([](nlohmann::json& j) { j["allow_channel_commands"] = "true"; });
    REQUIRE_FALSE(Config::load().allow_channel_commands);
}
