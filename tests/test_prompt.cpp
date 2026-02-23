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
