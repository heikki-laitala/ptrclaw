#include <catch2/catch_test_macros.hpp>
#include "agent.hpp"
#include "plugin.hpp"
#include "tool.hpp"
#include <filesystem>
#include <fstream>
#include <unistd.h>

using namespace ptrclaw;

// ── Mock provider ────────────────────────────────────────────────

class SkillMockProvider : public Provider {
public:
    ChatResponse chat(const std::vector<ChatMessage>&,
                      const std::vector<ToolSpec>&,
                      const std::string&, double) override {
        return ChatResponse{"ok", {}};
    }
    std::string chat_simple(const std::string&, const std::string&,
                            const std::string&, double) override {
        return "ok";
    }
    bool supports_native_tools() const override { return true; }
    std::string provider_name() const override { return "mock"; }
};

struct SkillActivateFixture {
    std::string old_home;
    std::filesystem::path tmpdir;
    std::unique_ptr<Tool> standalone_tool_;
    Agent agent;
    AgentAwareTool* skill_tool;

    SkillActivateFixture()
        : tmpdir(std::filesystem::temp_directory_path() /
                 ("ptrclaw_skill_act_" + std::to_string(getpid())))
        , agent(std::make_unique<SkillMockProvider>(),
                std::vector<std::unique_ptr<Tool>>{},
                []{ Config c; c.agent.max_tool_iterations = 5;
                    c.memory.backend = "none"; return c; }())
        , skill_tool(nullptr)
    {
        // Redirect HOME so load_skills finds our temp skills dir
        old_home = std::getenv("HOME") ? std::getenv("HOME") : "";
        std::filesystem::create_directories(tmpdir / ".ptrclaw" / "skills");
        setenv("HOME", tmpdir.c_str(), 1);

        {
            std::ofstream f(tmpdir / ".ptrclaw" / "skills" / "review.md");
            f << "---\nname: review\ntools: [file_read]\n---\nReview.\n";
        }
        {
            std::ofstream f(tmpdir / ".ptrclaw" / "skills" / "debug.md");
            f << "---\nname: debug\ntools: [shell]\n---\nDebug.\n";
        }

        // Get a standalone skill_activate tool and wire it to our agent
        auto tools = create_builtin_tools();
        for (auto& t : tools) {
            if (t->tool_name() == "skill_activate") {
                skill_tool = static_cast<AgentAwareTool*>(t.get());
                skill_tool->set_agent(&agent);
                standalone_tool_ = std::move(t);
                break;
            }
        }
    }

    ~SkillActivateFixture() noexcept {
        setenv("HOME", old_home.c_str(), 1);
        std::error_code ec;
        std::filesystem::remove_all(tmpdir, ec);
    }

    SkillActivateFixture(const SkillActivateFixture&) = delete;
    SkillActivateFixture& operator=(const SkillActivateFixture&) = delete;
};

// ── Tests ────────────────────────────────────────────────────────

TEST_CASE("skill_activate: no agent returns error", "[skill_activate]") {
    auto tools = create_builtin_tools();
    AgentAwareTool* tool = nullptr;
    for (auto& t : tools) {
        if (t->tool_name() == "skill_activate") {
            tool = static_cast<AgentAwareTool*>(t.get());
            break;
        }
    }
    REQUIRE(tool != nullptr);
    auto result = tool->execute(R"({"name": "test"})");
    REQUIRE_FALSE(result.success);
    REQUIRE(result.output.find("Agent not available") != std::string::npos);
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
