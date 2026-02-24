#include <catch2/catch_test_macros.hpp>
#include "prompt.hpp"

using namespace ptrclaw;

// ── Mock tool for prompt tests ──────────────────────────────────

class PromptMockTool : public Tool {
public:
    ToolResult execute(const std::string&) override { return {true, ""}; }
    std::string tool_name() const override { return "test_tool"; }
    std::string description() const override { return "A test tool"; }
    std::string parameters_json() const override { return R"({"type":"object"})"; }
};

// ── build_system_prompt ─────────────────────────────────────────

TEST_CASE("build_system_prompt: contains PtrClaw identity", "[prompt]") {
    std::vector<std::unique_ptr<Tool>> tools;
    auto result = build_system_prompt(tools, false);
    REQUIRE(result.find("PtrClaw") != std::string::npos);
}

TEST_CASE("build_system_prompt: contains working directory", "[prompt]") {
    std::vector<std::unique_ptr<Tool>> tools;
    auto result = build_system_prompt(tools, false);
    REQUIRE(result.find("Working directory:") != std::string::npos);
}

TEST_CASE("build_system_prompt: native provider shows tool summary", "[prompt]") {
    std::vector<std::unique_ptr<Tool>> tools;
    tools.push_back(std::make_unique<PromptMockTool>());
    auto result = build_system_prompt(tools, false);
    REQUIRE(result.find("Available tools:") == std::string::npos);
    REQUIRE(result.find("test_tool") != std::string::npos);
    REQUIRE(result.find("A test tool") != std::string::npos);
    REQUIRE(result.find("Use tools proactively") != std::string::npos);
    REQUIRE(result.find("tool_call") == std::string::npos);
}

TEST_CASE("build_system_prompt: XML provider shows full tool schemas", "[prompt]") {
    std::vector<std::unique_ptr<Tool>> tools;
    tools.push_back(std::make_unique<PromptMockTool>());
    auto result = build_system_prompt(tools, true);
    REQUIRE(result.find("Available tools:") != std::string::npos);
    REQUIRE(result.find("test_tool") != std::string::npos);
    REQUIRE(result.find("A test tool") != std::string::npos);
    REQUIRE(result.find("tool_call") != std::string::npos);
}

TEST_CASE("build_system_prompt: empty tool list omits tool section", "[prompt]") {
    std::vector<std::unique_ptr<Tool>> tools;
    auto result = build_system_prompt(tools, true);
    REQUIRE(result.find("tools") == std::string::npos);
    REQUIRE(result.find("tool_call") == std::string::npos);
}

TEST_CASE("build_system_prompt: multiple tools listed", "[prompt]") {
    std::vector<std::unique_ptr<Tool>> tools;
    tools.push_back(std::make_unique<PromptMockTool>());
    tools.push_back(std::make_unique<PromptMockTool>());
    auto result = build_system_prompt(tools, true);
    // "test_tool" should appear twice (once per tool)
    auto first = result.find("- test_tool");
    REQUIRE(first != std::string::npos);
    auto second = result.find("- test_tool", first + 1);
    REQUIRE(second != std::string::npos);
}

TEST_CASE("build_system_prompt: includes style adaptation instruction", "[prompt]") {
    std::vector<std::unique_ptr<Tool>> tools;
    auto result = build_system_prompt(tools, false);
    REQUIRE(result.find("Adapt your communication style") != std::string::npos);
}

TEST_CASE("build_system_prompt: includes tool call style section", "[prompt]") {
    std::vector<std::unique_ptr<Tool>> tools;
    tools.push_back(std::make_unique<PromptMockTool>());
    auto result = build_system_prompt(tools, false);
    REQUIRE(result.find("Tool Call Style") != std::string::npos);
    REQUIRE(result.find("Do not narrate routine") != std::string::npos);
}

TEST_CASE("build_system_prompt: includes safety section", "[prompt]") {
    std::vector<std::unique_ptr<Tool>> tools;
    auto result = build_system_prompt(tools, false);
    REQUIRE(result.find("## Safety") != std::string::npos);
    REQUIRE(result.find("self-preservation") != std::string::npos);
}

TEST_CASE("build_system_prompt: includes runtime info", "[prompt]") {
    std::vector<std::unique_ptr<Tool>> tools;
    RuntimeInfo runtime{"claude-sonnet-4", "anthropic", "telegram", "", ""};
    auto result = build_system_prompt(tools, false, false, nullptr, runtime);
    REQUIRE(result.find("## Runtime") != std::string::npos);
    REQUIRE(result.find("claude-sonnet-4") != std::string::npos);
    REQUIRE(result.find("anthropic") != std::string::npos);
    REQUIRE(result.find("telegram") != std::string::npos);
}

TEST_CASE("build_system_prompt: silent replies only with channel", "[prompt]") {
    std::vector<std::unique_ptr<Tool>> tools;
    // No channel — no silent replies
    auto result = build_system_prompt(tools, false);
    REQUIRE(result.find("[SILENT]") == std::string::npos);

    // With channel — has silent replies
    RuntimeInfo runtime{"", "", "telegram", "", ""};
    auto result2 = build_system_prompt(tools, false, false, nullptr, runtime);
    REQUIRE(result2.find("[SILENT]") != std::string::npos);
}

TEST_CASE("build_system_prompt: workspace section present", "[prompt]") {
    std::vector<std::unique_ptr<Tool>> tools;
    auto result = build_system_prompt(tools, false);
    REQUIRE(result.find("## Workspace") != std::string::npos);
}

TEST_CASE("build_system_prompt: includes binary path and session", "[prompt]") {
    std::vector<std::unique_ptr<Tool>> tools;
    RuntimeInfo runtime{"model", "provider", "telegram",
                        "/usr/local/bin/ptrclaw", "123456789"};
    auto result = build_system_prompt(tools, false, false, nullptr, runtime);
    REQUIRE(result.find("Binary: /usr/local/bin/ptrclaw") != std::string::npos);
    REQUIRE(result.find("Session: 123456789") != std::string::npos);
}

// Mock cron tool for scheduling hint test
class MockCronTool : public Tool {
public:
    ToolResult execute(const std::string&) override { return {true, ""}; }
    std::string tool_name() const override { return "cron"; }
    std::string description() const override { return "cron tool"; }
    std::string parameters_json() const override { return R"({"type":"object"})"; }
};

TEST_CASE("build_system_prompt: scheduling hint with cron tool", "[prompt]") {
    std::vector<std::unique_ptr<Tool>> tools;
    tools.push_back(std::make_unique<MockCronTool>());
    RuntimeInfo runtime{"model", "provider", "telegram",
                        "/usr/local/bin/ptrclaw", "123456789"};
    auto result = build_system_prompt(tools, false, false, nullptr, runtime);
    REQUIRE(result.find("## Scheduled Tasks") != std::string::npos);
    REQUIRE(result.find("/usr/local/bin/ptrclaw -m") != std::string::npos);
    REQUIRE(result.find("--notify telegram:123456789") != std::string::npos);
}

TEST_CASE("build_system_prompt: no scheduling hint without binary path", "[prompt]") {
    std::vector<std::unique_ptr<Tool>> tools;
    tools.push_back(std::make_unique<MockCronTool>());
    RuntimeInfo runtime{"model", "provider", "telegram", "", ""};
    auto result = build_system_prompt(tools, false, false, nullptr, runtime);
    REQUIRE(result.find("## Scheduled Tasks") == std::string::npos);
}
