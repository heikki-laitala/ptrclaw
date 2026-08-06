#include <catch2/catch_test_macros.hpp>
#include "plugin.hpp"
#include "tool.hpp"
#include "tools/serving_file.hpp"
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace ptrclaw;

// Compiled only in the serving configuration (-Dwith_serving=true -Dwith_tools=false),
// because what it asserts is which tools are ABSENT — a claim no other build can make.
// The counterpart for the read-only profile is tests/test_file_read_only.cpp.

namespace {

// Same idiom as tests/test_file_read_only.cpp: the registry exposes tool_names(), not a
// membership query.
bool registered(const std::string& name) {
    const auto names = PluginRegistry::instance().tool_names();
    return std::find(names.begin(), names.end(), name) != names.end();
}

const Tool* find_tool(const std::vector<std::unique_ptr<Tool>>& tools,
                      const std::string& name) {
    for (const auto& tool : tools) {
        if (tool->tool_name() == name) return tool.get();
    }
    return nullptr;
}

} // namespace

// The reason the profile exists: a pod where every session shares one filesystem and the
// process holds provider credentials cannot offer a tool that runs arbitrary commands.
TEST_CASE("serving profile: shell and cron are not in the binary", "[serving][profile]") {
    REQUIRE_FALSE(registered("shell"));
    REQUIRE_FALSE(registered("cron"));
}

TEST_CASE("serving profile: the mutating unscoped tools are absent", "[serving][profile]") {
    REQUIRE_FALSE(registered("file_edit"));
    REQUIRE_FALSE(registered("skill_activate"));
}

TEST_CASE("serving profile: file_read and file_write are the scoped ones",
          "[serving][profile]") {
    REQUIRE(registered("file_read"));
    REQUIRE(registered("file_write"));

    // Identity, not just presence: the unscoped tools carry the same names, so the test
    // has to prove which implementation a session actually gets.
    auto tools = create_builtin_tools();
    const Tool* read = find_tool(tools, "file_read");
    const Tool* write = find_tool(tools, "file_write");
    REQUIRE(read != nullptr);
    REQUIRE(write != nullptr);
    REQUIRE(dynamic_cast<const ScopedFileReadTool*>(read) != nullptr);
    REQUIRE(dynamic_cast<const ScopedFileWriteTool*>(write) != nullptr);
}

// Unwired, they refuse everything. A session that never received a workspace must not fall
// back to the process cwd, which is where the pod's own config lives.
TEST_CASE("serving profile: scoped tools fail closed before wiring", "[serving][profile]") {
    auto tools = create_builtin_tools();
    for (auto& tool : tools) {
        if (tool->tool_name() != "file_read") continue;
        auto result = tool->execute(R"({"path":"/etc/passwd"})");
        REQUIRE_FALSE(result.success);
    }
}
