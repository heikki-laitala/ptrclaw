#include <catch2/catch_test_macros.hpp>
#include "agent.hpp"
#include "plugin.hpp"
#include "tool_manager.hpp"
#include "event_bus.hpp"
#include "test_helpers.hpp"

using namespace ptrclaw;

struct SkillActivateFixture {
    HomeGuard home;
    EventBus bus;
    std::unique_ptr<ToolManager> tool_mgr;
    Agent agent;
    Tool* skill_tool;

    SkillActivateFixture()
        : agent(std::make_unique<StubProvider>(),
                []{ Config c; c.agent.max_tool_iterations = 5;
                    c.memory.backend = "none"; return c; }())
        , skill_tool(nullptr)
    {
        home.add_skill("review.md",
            "---\nname: review\n---\nReview.\n");
        home.add_skill("debug.md",
            "---\nname: debug\n---\nDebug.\n");

        auto tools = create_builtin_tools();
        for (const auto& t : tools) {
            if (t->tool_name() == "skill_activate") {
                skill_tool = t.get();
            }
        }

        Config c;
        c.agent.max_tool_iterations = 5;
        c.memory.backend = "none";
        tool_mgr = std::make_unique<ToolManager>(std::move(tools), c, bus);
        agent.set_event_bus(&bus);
        tool_mgr->wire_memory(agent.memory());
        tool_mgr->publish_tool_specs();
    }
};

// ── Tests ────────────────────────────────────────────────────────

TEST_CASE("skill_activate: no event bus returns error", "[skill_activate]") {
    auto tools = create_builtin_tools();
    Tool* tool = nullptr;
    for (const auto& t : tools) {
        if (t->tool_name() == "skill_activate") {
            tool = t.get();
            break;
        }
    }
    REQUIRE(tool != nullptr);
    auto result = tool->execute(R"({"name": "test"})");
    REQUIRE_FALSE(result.success);
    REQUIRE(result.output.find("EventBus not available") != std::string::npos);
}

TEST_CASE("skill_activate: invalid JSON", "[skill_activate]") {
    SkillActivateFixture f;
    auto result = f.skill_tool->execute("not json");
    REQUIRE_FALSE(result.success);
    REQUIRE(result.output.find("Invalid JSON") != std::string::npos);
}

TEST_CASE("skill_activate: missing name parameter", "[skill_activate]") {
    SkillActivateFixture f;
    auto result = f.skill_tool->execute(R"({})");
    REQUIRE_FALSE(result.success);
    REQUIRE(result.output.find("Missing required parameter") != std::string::npos);
}

TEST_CASE("skill_activate: activate valid skill", "[skill_activate]") {
    SkillActivateFixture f;
    auto result = f.skill_tool->execute(R"({"name": "review"})");
    REQUIRE(result.success);
    REQUIRE(result.output.find("review") != std::string::npos);
    REQUIRE(result.output.find("activated") != std::string::npos);
    REQUIRE(f.agent.active_skill_name() == "review");
}

TEST_CASE("skill_activate: deactivate with off", "[skill_activate]") {
    SkillActivateFixture f;
    f.skill_tool->execute(R"({"name": "review"})");
    auto result = f.skill_tool->execute(R"({"name": "off"})");
    REQUIRE(result.success);
    REQUIRE(result.output.find("deactivated") != std::string::npos);
    REQUIRE(f.agent.active_skill_name().empty());
}

TEST_CASE("skill_activate: deactivate with none", "[skill_activate]") {
    SkillActivateFixture f;
    f.skill_tool->execute(R"({"name": "debug"})");
    auto result = f.skill_tool->execute(R"({"name": "none"})");
    REQUIRE(result.success);
    REQUIRE(result.output.find("deactivated") != std::string::npos);
}

TEST_CASE("skill_activate: unknown skill lists available", "[skill_activate]") {
    SkillActivateFixture f;
    auto result = f.skill_tool->execute(R"({"name": "nonexistent"})");
    REQUIRE_FALSE(result.success);
    REQUIRE(result.output.find("Unknown skill") != std::string::npos);
    REQUIRE(result.output.find("review") != std::string::npos);
    REQUIRE(result.output.find("debug") != std::string::npos);
}
