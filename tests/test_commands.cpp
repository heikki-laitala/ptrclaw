#include <catch2/catch_test_macros.hpp>
#include "agent.hpp"
#include "commands.hpp"
#include "memory/json_memory.hpp"
#include "test_helpers.hpp"
#include <unistd.h>

using namespace ptrclaw;

static Agent make_cmd_agent() {
    auto provider = std::make_unique<StubProvider>();
    Config cfg;
    cfg.agent.max_tool_iterations = 5;
    cfg.memory.backend = "none";
    return Agent(std::move(provider), cfg);
}

// ── cmd_status ───────────────────────────────────────────────────

TEST_CASE("cmd_status: returns provider and model info", "[commands]") {
    auto agent = make_cmd_agent();
    auto result = cmd_status(agent);
    REQUIRE(result.find("Provider: stub") != std::string::npos);
    REQUIRE(result.find("Model:") != std::string::npos);
    REQUIRE(result.find("History:") != std::string::npos);
    REQUIRE(result.find("Estimated tokens:") != std::string::npos);
}

// ── cmd_memory ───────────────────────────────────────────────────

TEST_CASE("cmd_memory: disabled when no memory", "[commands]") {
    auto agent = make_cmd_agent();
    REQUIRE(cmd_memory(agent) == "Memory: disabled");
}

TEST_CASE("cmd_memory: shows backend stats with memory", "[commands]") {
    auto agent = make_cmd_agent();
    std::string path = "/tmp/ptrclaw_test_cmd_mem_" + std::to_string(getpid()) + ".json";
    auto memory = std::make_unique<JsonMemory>(path);
    agent.set_memory(std::move(memory));

    auto result = cmd_memory(agent);
    REQUIRE(result.find("Memory backend: json") != std::string::npos);
    REQUIRE(result.find("Core:") != std::string::npos);
    REQUIRE(result.find("Knowledge:") != std::string::npos);
    REQUIRE(result.find("Conversation:") != std::string::npos);
    REQUIRE(result.find("Total:") != std::string::npos);

    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(std::string(path + ".tmp"), ec);
}

// ── cmd_soul ─────────────────────────────────────────────────────

TEST_CASE("cmd_soul: returns unknown command when dev is false", "[commands]") {
    auto agent = make_cmd_agent();
    REQUIRE(cmd_soul(agent, false) == "Unknown command: /soul");
}

TEST_CASE("cmd_soul: returns hatch prompt when dev and no soul data", "[commands]") {
    auto agent = make_cmd_agent();
    auto result = cmd_soul(agent, true);
    REQUIRE(result.find("hatch") != std::string::npos);
}

// ── cmd_models ──────────────────────────────────────────────────

TEST_CASE("cmd_models: shows current provider and model", "[commands]") {
    auto agent = make_cmd_agent();
    Config cfg;
    cfg.providers["anthropic"].api_key = "test-key";
    cfg.providers["openai"].api_key = "sk-test";

    auto result = cmd_models(agent, cfg);
    REQUIRE(result.find("Current: stub") != std::string::npos);
    REQUIRE(result.find("Providers:") != std::string::npos);
    REQUIRE(result.find("anthropic") != std::string::npos);
    REQUIRE(result.find("openai") != std::string::npos);
    REQUIRE(result.find("API key") != std::string::npos);
    REQUIRE(result.find("/provider") != std::string::npos);
}

TEST_CASE("cmd_models: shows OAuth for openai with oauth token", "[commands]") {
    auto agent = make_cmd_agent();
    Config cfg;
    cfg.providers["openai"].api_key = "sk-test";
    cfg.providers["openai"].oauth_access_token = "token";

    auto result = cmd_models(agent, cfg);
    REQUIRE(result.find("OAuth") != std::string::npos);
}

TEST_CASE("cmd_models: empty providers", "[commands]") {
    auto agent = make_cmd_agent();
    Config cfg;

    auto result = cmd_models(agent, cfg);
    REQUIRE(result.find("Current:") != std::string::npos);
    REQUIRE(result.find("Providers:") != std::string::npos);
}

// ── cmd_skill ────────────────────────────────────────────────────

TEST_CASE("cmd_skill: no skills available", "[commands]") {
    HomeGuard home;
    auto agent = make_cmd_agent();
    auto result = cmd_skill("", agent);
    REQUIRE(result.find("No skills available") != std::string::npos);
}

TEST_CASE("cmd_skill: lists skills with off tag", "[commands]") {
    HomeGuard home;
    home.add_skill("review.md",
        "---\nname: review\ndescription: Code review\ntools: [file_read]\n---\nReview code.\n");
    auto agent = make_cmd_agent();

    auto result = cmd_skill("", agent);
    REQUIRE(result.find("review") != std::string::npos);
    REQUIRE(result.find("[off]") != std::string::npos);
    REQUIRE(result.find("Code review") != std::string::npos);
    REQUIRE(result.find("Activate:") != std::string::npos);
    REQUIRE(result.find("Deactivate:") != std::string::npos);
}

TEST_CASE("cmd_skill: activate shows active tag", "[commands]") {
    HomeGuard home;
    home.add_skill("debug.md",
        "---\nname: debug\n---\nDebug.\n");
    auto agent = make_cmd_agent();
    agent.load_skills();
    REQUIRE(agent.activate_skill("debug"));

    auto result = cmd_skill("", agent);
    REQUIRE(result.find("* debug") != std::string::npos);
    REQUIRE(result.find("[active]") != std::string::npos);
}

TEST_CASE("cmd_skill: activate and deactivate by name", "[commands]") {
    HomeGuard home;
    home.add_skill("test.md", "---\nname: test\n---\nTest skill.\n");
    auto agent = make_cmd_agent();

    REQUIRE(cmd_skill("test", agent).find("activated") != std::string::npos);
    REQUIRE(cmd_skill("off", agent) == "Skill deactivated");
    REQUIRE(cmd_skill("nonexistent", agent).find("Unknown skill") != std::string::npos);
}
