// The read-only tool configuration: -Dwith_tools=false -Dwith_file_read=true.
//
// Compiled ONLY in that configuration (see meson.build), because what it asserts is which
// tools are absent — a claim the full-tools build cannot make and would fail. The absence
// is the whole point of the option: an agent that reads reference material to answer
// questions should not also be able to rewrite the filesystem or run commands, and before
// this option existed it had to be given both to get either.

#include <catch2/catch_test_macros.hpp>

#include "plugin.hpp"
#include "tools/file_read.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

using namespace ptrclaw;

namespace {

bool registered(const std::string& name) {
    const auto names = PluginRegistry::instance().tool_names();
    return std::find(names.begin(), names.end(), name) != names.end();
}

} // namespace

TEST_CASE("read-only tools: file_read is registered", "[file_read_only]") {
    REQUIRE(registered("file_read"));
}

TEST_CASE("read-only tools: nothing that mutates is registered", "[file_read_only]") {
    // Named individually rather than counted: a count passes just as well if the set is
    // empty, and "no tools at all" is a different build from the one under test.
    CHECK_FALSE(registered("file_write"));
    CHECK_FALSE(registered("file_edit"));
    CHECK_FALSE(registered("shell"));
    CHECK_FALSE(registered("cron"));
    CHECK_FALSE(registered("skill_activate"));
}

TEST_CASE("read-only tools: file_read still reads a file", "[file_read_only]") {
    // Registration is not usefulness. Compiling the tool in but leaving it broken would
    // satisfy every assertion above.
    const auto path = std::filesystem::temp_directory_path() / "ptrclaw_file_read_only.txt";
    {
        std::ofstream f(path);
        f << "breakfast is at seven\n";
    }

    FileReadTool tool;
    const auto result = tool.execute(nlohmann::json{{"path", path.string()}}.dump());
    std::filesystem::remove(path);

    REQUIRE(result.success);
    REQUIRE(result.output.find("breakfast is at seven") != std::string::npos);
}
