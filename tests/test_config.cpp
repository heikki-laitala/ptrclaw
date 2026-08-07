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
    // Not a literal: the default provider tracks which ones the binary was built with, so
    // a trimmed build has a different — and equally sensible — answer. What it must never
    // be is empty. Which provider, per build, is asserted further down.
    REQUIRE_FALSE(cfg.provider.empty());
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
    // The hour that used to be hard-coded at the eviction call site. Still pinned for the
    // personal agent, so making it configurable cannot quietly change what an unconfigured
    // deployment does — a serving build deliberately lets go sooner, asserted separately.
#ifdef PTRCLAW_HAS_SERVING
    REQUIRE(ac.session_max_idle_seconds == 900);
#else
    REQUIRE(ac.session_max_idle_seconds == 3600);
#endif
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

// ── configured persona ──────────────────────────────────────────

// Three parts, matching what hatching writes into memory, so a configured pod and a hatched
// personal agent describe themselves identically.
TEST_CASE("PersonaConfig: absent by default", "[config][persona]") {
    Config cfg;
    REQUIRE(cfg.agent.persona.identity.empty());
    REQUIRE(cfg.agent.persona.user.empty());
    REQUIRE(cfg.agent.persona.philosophy.empty());
    REQUIRE(cfg.agent.persona.empty());
}

TEST_CASE("Config::load: reads all three persona parts", "[config][persona]") {
    ConfigTestGuard g;
    REQUIRE_FALSE(g.dir.empty());
    g.write_config(R"({
        "agent": { "persona": {
            "identity": "You are Atlas, a terse research assistant.",
            "user": "Heikki, in Helsinki, prefers short answers.",
            "philosophy": "Say the useful thing first."
        } }
    })");

    Config cfg = Config::load();
    REQUIRE(cfg.agent.persona.identity == "You are Atlas, a terse research assistant.");
    REQUIRE(cfg.agent.persona.user == "Heikki, in Helsinki, prefers short answers.");
    REQUIRE(cfg.agent.persona.philosophy == "Say the useful thing first.");
    REQUIRE_FALSE(cfg.agent.persona.empty());
}

// Identity alone is enough — the other two are optional in memory too.
TEST_CASE("Config::load: an identity-only persona counts", "[config][persona]") {
    ConfigTestGuard g;
    REQUIRE_FALSE(g.dir.empty());
    g.write_config(R"({"agent": {"persona": {"identity": "You are Atlas."}}})");

    Config cfg = Config::load();
    REQUIRE(cfg.agent.persona.identity == "You are Atlas.");
    REQUIRE_FALSE(cfg.agent.persona.empty());
}

// Without an identity there is nothing to introduce, so the block would render headless.
TEST_CASE("Config::load: user or philosophy alone is not a persona", "[config][persona]") {
    ConfigTestGuard g;
    REQUIRE_FALSE(g.dir.empty());
    g.write_config(R"({"agent": {"persona": {"user": "Heikki", "philosophy": "Be brief"}}})");
    REQUIRE(Config::load().agent.persona.empty());
}

TEST_CASE("Config::load: wrong persona types keep the defaults", "[config][persona]") {
    ConfigTestGuard g;
    REQUIRE_FALSE(g.dir.empty());
    g.write_config(R"({"agent": {"persona": {"identity": 42, "user": null}}})");
    REQUIRE(Config::load().agent.persona.empty());
}

TEST_CASE("Config::defaults_json: carries no persona", "[config][persona]") {
    // A personal install must not be migrated into having one.
    auto defaults = Config::defaults_json();
    REQUIRE_FALSE(defaults["agent"].contains("persona"));
}

// ── serving profile ─────────────────────────────────────────────

// A pod's per-session store is write-only in practice: the session records facts, ends, and
// nobody returns to that id — so it pays embedding calls, synthesis calls and three files on
// disk for something never read again. Off by default there; an operator serving returning
// conversations turns it back on.
TEST_CASE("MemoryConfig: a serving build has no memory backend by default",
          "[config][serving]") {
    Config cfg;
#ifdef PTRCLAW_HAS_SERVING
    REQUIRE(cfg.memory.backend == "none");
#else
    REQUIRE(cfg.memory.backend != "none");
#endif
}

// Through load(), which is the path a deployment takes: it merges defaults_json() into the
// file and parses the result, so a hardcoded backend there would overwrite the build default
// — the mistake caught in review on the isolation key.
TEST_CASE("MemoryConfig: the backend default survives Config::load", "[config][serving]") {
    ConfigTestGuard g;
    REQUIRE_FALSE(g.dir.empty());
    g.write_config("{}");

    Config cfg = Config::load();
#ifdef PTRCLAW_HAS_SERVING
    REQUIRE(cfg.memory.backend == "none");
#else
    REQUIRE(cfg.memory.backend != "none");
#endif
}

TEST_CASE("MemoryConfig: defaults_json carries the build backend", "[config][serving]") {
    auto defaults = Config::defaults_json();
    REQUIRE(defaults["memory"].contains("backend"));
#ifdef PTRCLAW_HAS_SERVING
    REQUIRE(defaults["memory"]["backend"] == "none");
#else
    REQUIRE(defaults["memory"]["backend"] != "none");
#endif
}

// A pod that serves returning conversations says so and gets a store, isolated per session.
TEST_CASE("MemoryConfig: an explicit backend still wins", "[config][serving]") {
    ConfigTestGuard g;
    REQUIRE_FALSE(g.dir.empty());
    g.write_config(R"({"memory": {"backend": "json"}})");
    REQUIRE(Config::load().memory.backend == "json");
}

// A serving build fences the filesystem per session; leaving memory shared would pair that
// with one store every tenant reads and writes, so the default follows the build.
TEST_CASE("MemoryConfig: a serving build isolates memory by default", "[config][serving]") {
    Config cfg;
#ifdef PTRCLAW_HAS_SERVING
    REQUIRE(cfg.memory.isolation == "session");
#else
    REQUIRE(cfg.memory.isolation == "shared");
#endif
}

// Through Config::load(), which is the only path a deployment takes. The struct default
// alone proves nothing: load() merges defaults_json() into the file and then parses the
// result, so a hardcoded "shared" there would overwrite the build's default and quietly
// share one memory store across every session.
TEST_CASE("MemoryConfig: the build default survives Config::load", "[config][serving]") {
    ConfigTestGuard g;
    REQUIRE_FALSE(g.dir.empty());
    g.write_config("{}");   // no memory section at all

    Config cfg = Config::load();
#ifdef PTRCLAW_HAS_SERVING
    REQUIRE(cfg.memory.isolation == "session");
#else
    REQUIRE(cfg.memory.isolation == "shared");
#endif
}

// And through the generated default file, which is what a first run writes.
TEST_CASE("MemoryConfig: defaults_json carries the build default", "[config][serving]") {
    auto defaults = Config::defaults_json();
    REQUIRE(defaults["memory"].contains("isolation"));
#ifdef PTRCLAW_HAS_SERVING
    REQUIRE(defaults["memory"]["isolation"] == "session");
#else
    REQUIRE(defaults["memory"]["isolation"] == "shared");
#endif
}

// Still an explicit choice: an operator who writes "shared" gets it, because a pod serving
// one tenant's own tasks may legitimately want a common store.
TEST_CASE("MemoryConfig: an explicit isolation still wins", "[config][serving]") {
    ConfigTestGuard g;
    REQUIRE_FALSE(g.dir.empty());
    g.write_config(R"({"memory": {"isolation": "shared"}})");
    REQUIRE(Config::load().memory.isolation == "shared");
}

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
    // Against the build's default rather than a literal: what this asserts is that a bad
    // value is ignored, whichever default it falls back to.
    REQUIRE(Config::load().agent.session_max_idle_seconds ==
            AgentConfig{}.session_max_idle_seconds);
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
    // The subject is the fallback, not the provider's name — comparing against the struct
    // default keeps that true in a build compiled without Anthropic.
    REQUIRE(cfg.provider == Config{}.provider);
    REQUIRE(cfg.api_key_for("anthropic").empty());
}

TEST_CASE("Config::load: missing config file uses defaults", "[config]") {
    ConfigTestGuard g;
    REQUIRE_FALSE(g.dir.empty());

    Config cfg = Config::load();
    REQUIRE(cfg.provider == Config{}.provider);
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
    REQUIRE(j["provider"] == Config{}.provider);
    REQUIRE(j.contains("providers"));
    REQUIRE(j["providers"].contains("anthropic"));
    REQUIRE(j.contains("agent"));
    REQUIRE(j["agent"].contains("max_tool_iterations"));
    REQUIRE(j.contains("memory"));
    REQUIRE(j["memory"].contains("backend"));
    // The serving build writes "none": a pod's per-session store is never read again, so
    // the generated config must not hand it one.
#ifdef PTRCLAW_HAS_SERVING
    REQUIRE(j["memory"]["backend"] == "none");
#elif defined(PTRCLAW_HAS_SQLITE_MEMORY)
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
    // The serving build writes "none": a pod's per-session store is never read again, so
    // the generated config must not hand it one.
#ifdef PTRCLAW_HAS_SERVING
    REQUIRE(j["memory"]["backend"] == "none");
#elif defined(PTRCLAW_HAS_SQLITE_MEMORY)
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

// ── The default provider has to exist in the binary ──────────────
//
// A pod built for one provider is the point of the serving profile, but the default
// provider is a compile-time constant that a trimmed build can no longer create. Left
// alone, a config that simply omits "provider" would fail at startup with "Unknown
// provider: anthropic" — a binary whose out-of-the-box default cannot work.

TEST_CASE("Config: the default provider is compiled into this build", "[config]") {
    Config cfg;
    REQUIRE_FALSE(cfg.provider.empty());
#ifdef PTRCLAW_HAS_ANTHROPIC
    REQUIRE(cfg.provider == "anthropic");
#else
    REQUIRE(cfg.provider != "anthropic");
#endif
}

TEST_CASE("Config: the default model belongs to the default provider", "[config]") {
    Config cfg;
    // The compatible provider is the exception and deliberately so: the endpoint and its
    // model names both belong to the operator, so there is nothing to guess.
#if defined(PTRCLAW_HAS_COMPATIBLE) && !defined(PTRCLAW_HAS_ANTHROPIC) && \
    !defined(PTRCLAW_HAS_OPENAI_PROVIDER) && !defined(PTRCLAW_HAS_OPENROUTER) && \
    !defined(PTRCLAW_HAS_OLLAMA)
    REQUIRE(cfg.model.empty());
    return;
#endif
    REQUIRE_FALSE(cfg.model.empty());
    // Pairing matters as much as the provider itself: an OpenAI-only build defaulting to a
    // Claude model would authenticate fine and then be refused by the API.
#ifdef PTRCLAW_HAS_ANTHROPIC
    REQUIRE(cfg.model.rfind("claude", 0) == 0);
#else
    REQUIRE(cfg.model.rfind("claude", 0) != 0);
#endif
}

// Through load(), for the reason the memory-backend tests give: defaults_json() is merged
// into the config file and parsed back, so a hardcoded provider there would overwrite the
// build's default and hand a trimmed pod a provider it cannot construct.
TEST_CASE("Config: the provider default survives Config::load", "[config]") {
    ConfigTestGuard g;
    REQUIRE_FALSE(g.dir.empty());
    g.write_config("{}");

    Config cfg = Config::load();
#ifdef PTRCLAW_HAS_ANTHROPIC
    REQUIRE(cfg.provider == "anthropic");
#else
    REQUIRE(cfg.provider != "anthropic");
#endif
}

TEST_CASE("Config: defaults_json carries the build's provider", "[config]") {
    auto defaults = Config::defaults_json();
    REQUIRE(defaults.contains("provider"));
#ifdef PTRCLAW_HAS_ANTHROPIC
    REQUIRE(defaults["provider"] == "anthropic");
#else
    REQUIRE(defaults["provider"] != "anthropic");
#endif
}

// ── Capacity defaults for a pod ──────────────────────────────────
//
// The personal agent talks to one person: one worker, no session cap, and an hour of idle
// grace are right for it. A pod serves many conversations at once, and each of those
// defaults is wrong there in a way that is invisible until load arrives — workers=1
// serialises every turn, and an unbounded session count is caller-driven memory growth.

TEST_CASE("Config: a serving build runs turns in parallel by default", "[config][serving]") {
    Config cfg;
#ifdef PTRCLAW_HAS_SERVING
    REQUIRE(cfg.workers > 1);
#else
    REQUIRE(cfg.workers == 1);
#endif
}

TEST_CASE("Config: a serving build bounds how many sessions it holds",
          "[config][serving]") {
    Config cfg;
#ifdef PTRCLAW_HAS_SERVING
    // Zero means unlimited, and with generated session ids the count is chosen by whoever
    // is calling — so a pod that never refuses grows until the kernel refuses for it.
    REQUIRE(cfg.agent.max_sessions > 0);
    // And it lets go sooner: the caller can push history back, so a session freed early
    // costs a reconstruction rather than a conversation.
    REQUIRE(cfg.agent.session_max_idle_seconds < 3600);
#else
    REQUIRE(cfg.agent.max_sessions == 0);
    REQUIRE(cfg.agent.session_max_idle_seconds == 3600);
#endif
}

// Through load(), for the reason the memory-backend tests give: defaults_json() is merged
// into the config file and parsed back, so a hardcoded worker count there would overwrite
// the build's default and serialise a pod's turns.
TEST_CASE("Config: the capacity defaults survive Config::load", "[config][serving]") {
    ConfigTestGuard g;
    REQUIRE_FALSE(g.dir.empty());
    g.write_config("{}");

    Config cfg = Config::load();
    Config defaults;
    REQUIRE(cfg.workers == defaults.workers);
    REQUIRE(cfg.agent.max_sessions == defaults.agent.max_sessions);
    REQUIRE(cfg.agent.session_max_idle_seconds == defaults.agent.session_max_idle_seconds);
}

TEST_CASE("Config: defaults_json carries the build's worker count", "[config][serving]") {
    auto defaults = Config::defaults_json();
    REQUIRE(defaults.contains("workers"));
    REQUIRE(defaults["workers"] == Config{}.workers);
}

// An explicit value always wins — these are starting points, not policy.
TEST_CASE("Config: explicit capacity settings beat the build defaults",
          "[config][serving]") {
    ConfigTestGuard g;
    REQUIRE_FALSE(g.dir.empty());
    g.write_config(R"({"workers": 3, "agent": {"max_sessions": 7,
                       "session_max_idle_seconds": 42}})");

    Config cfg = Config::load();
    REQUIRE(cfg.workers == 3);
    REQUIRE(cfg.agent.max_sessions == 7);
    REQUIRE(cfg.agent.session_max_idle_seconds == 42);
}

// The default provider must be one the build was *asked* for, not merely one whose code got
// linked. `opt_openai` in meson.build is the OR of with_openai, with_openrouter and
// with_compatible — because the latter two inherit OpenAIProvider — so an OpenRouter-only
// build still compiles and registers "openai". A default keyed on that would pick a
// provider the operator never enabled and fail with "No API key for openai".
TEST_CASE("Config: the default provider is one this build was asked for", "[config]") {
    Config cfg;
#if defined(PTRCLAW_HAS_ANTHROPIC)
    REQUIRE(cfg.provider == "anthropic");
#elif defined(PTRCLAW_HAS_OPENAI_PROVIDER)
    REQUIRE(cfg.provider == "openai");
#elif defined(PTRCLAW_HAS_OPENROUTER)
    REQUIRE(cfg.provider == "openrouter");
#elif defined(PTRCLAW_HAS_OLLAMA)
    REQUIRE(cfg.provider == "ollama");
#elif defined(PTRCLAW_HAS_COMPATIBLE)
    REQUIRE(cfg.provider == "compatible");
#else
    REQUIRE(cfg.provider.empty());  // nothing to talk to, and no default worth inventing
#endif
}

// The macro-free half of the same invariant: whatever the default is, this binary must be
// able to construct it.
TEST_CASE("Config: the default provider is registered", "[config]") {
    Config cfg;
    if (cfg.provider.empty()) return;  // a build with no providers at all
    REQUIRE(PluginRegistry::instance().has_provider(cfg.provider));
}

// ── The worker ceiling ──────────────────────────────────────────
//
// The cap silently rewrites a configured value, which is the worst kind of limit to get
// wrong: a pod asking for 512 workers ran with 64 and looked like it had a concurrency
// ceiling of its own. Measured, a worker costs ~15 KB resident — 1024 of them is ~21 MB,
// so the ceiling exists to catch a typo, not to ration memory.

TEST_CASE("Config: a large worker count is honoured, not silently reduced", "[config]") {
    ConfigTestGuard g;
    REQUIRE_FALSE(g.dir.empty());
    g.write_config(R"({"workers": 512})");
    REQUIRE(Config::load().workers == 512);
}

TEST_CASE("Config: workers are capped at the documented ceiling", "[config]") {
    ConfigTestGuard g;
    REQUIRE_FALSE(g.dir.empty());
    g.write_config(R"({"workers": 999999})");
    REQUIRE(Config::load().workers == kMaxWorkers);
}

TEST_CASE("Config: zero workers means one", "[config]") {
    // Zero would mean "run turns inline"; the pool already treats <= 1 that way, and a
    // configured 0 is far more likely to be a mistake than a request for it.
    ConfigTestGuard g;
    REQUIRE_FALSE(g.dir.empty());
    g.write_config(R"({"workers": 0})");
    REQUIRE(Config::load().workers == 1);
}
