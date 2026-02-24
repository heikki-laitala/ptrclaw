#include <catch2/catch_test_macros.hpp>
#include "tools/cron.hpp"

using namespace ptrclaw;

// ═══ CronTool: metadata ═════════════════════════════════════════

TEST_CASE("CronTool: tool_name is cron", "[tools][cron]") {
    CronTool tool;
    REQUIRE(tool.tool_name() == "cron");
    REQUIRE_FALSE(tool.description().empty());
    REQUIRE(tool.parameters_json().find("action") != std::string::npos);
}

// ═══ CronTool: argument validation ══════════════════════════════

TEST_CASE("CronTool: invalid JSON args", "[tools][cron]") {
    CronTool tool;
    auto result = tool.execute("not json");
    REQUIRE_FALSE(result.success);
    REQUIRE(result.output.find("parse") != std::string::npos);
}

TEST_CASE("CronTool: missing action", "[tools][cron]") {
    CronTool tool;
    auto result = tool.execute(R"({})");
    REQUIRE_FALSE(result.success);
    REQUIRE(result.output.find("action") != std::string::npos);
}

TEST_CASE("CronTool: unknown action", "[tools][cron]") {
    CronTool tool;
    auto result = tool.execute(R"({"action":"bogus"})");
    REQUIRE_FALSE(result.success);
    REQUIRE(result.output.find("Unknown action") != std::string::npos);
}

TEST_CASE("CronTool: add missing schedule", "[tools][cron]") {
    CronTool tool;
    auto result = tool.execute(R"({"action":"add","command":"echo hi","label":"test"})");
    REQUIRE_FALSE(result.success);
    REQUIRE(result.output.find("schedule") != std::string::npos);
}

TEST_CASE("CronTool: add missing command", "[tools][cron]") {
    CronTool tool;
    auto result = tool.execute(R"({"action":"add","schedule":"0 * * * *","label":"test"})");
    REQUIRE_FALSE(result.success);
    REQUIRE(result.output.find("command") != std::string::npos);
}

TEST_CASE("CronTool: add missing label", "[tools][cron]") {
    CronTool tool;
    auto result = tool.execute(R"({"action":"add","schedule":"0 * * * *","command":"echo hi"})");
    REQUIRE_FALSE(result.success);
    REQUIRE(result.output.find("label") != std::string::npos);
}

TEST_CASE("CronTool: remove missing label", "[tools][cron]") {
    CronTool tool;
    auto result = tool.execute(R"({"action":"remove"})");
    REQUIRE_FALSE(result.success);
    REQUIRE(result.output.find("label") != std::string::npos);
}

// ═══ CronTool: schedule validation ══════════════════════════════

TEST_CASE("CronTool: validate_schedule accepts valid expressions", "[tools][cron]") {
    CronTool tool;

    // Valid: basic hourly
    auto r1 = tool.execute(R"({"action":"add","schedule":"0 * * * *","command":"echo ok","label":"__ptrclaw_test_valid1"})");
    // We check it didn't fail on validation — it may fail on crontab write in CI, that's fine
    if (!r1.success) {
        // If it failed, it should NOT be a validation error
        REQUIRE(r1.output.find("Invalid cron schedule") == std::string::npos);
    }

    // Valid: every 5 min during work hours on weekdays
    auto r2 = tool.execute(R"({"action":"add","schedule":"*/5 9-17 * * 1-5","command":"echo ok","label":"__ptrclaw_test_valid2"})");
    if (!r2.success) {
        REQUIRE(r2.output.find("Invalid cron schedule") == std::string::npos);
    }

    // Clean up (ignore errors — may not have been added)
    tool.execute(R"({"action":"remove","label":"__ptrclaw_test_valid1"})");
    tool.execute(R"({"action":"remove","label":"__ptrclaw_test_valid2"})");
}

TEST_CASE("CronTool: validate_schedule rejects invalid expressions", "[tools][cron]") {
    CronTool tool;

    // Too few fields
    auto r1 = tool.execute(R"({"action":"add","schedule":"0 * *","command":"echo x","label":"bad1"})");
    REQUIRE_FALSE(r1.success);
    REQUIRE(r1.output.find("Invalid cron schedule") != std::string::npos);

    // Too many fields
    auto r2 = tool.execute(R"({"action":"add","schedule":"0 * * * * *","command":"echo x","label":"bad2"})");
    REQUIRE_FALSE(r2.success);
    REQUIRE(r2.output.find("Invalid cron schedule") != std::string::npos);

    // Shell metacharacters
    auto r3 = tool.execute(R"({"action":"add","schedule":"0 * * * ; rm -rf /","command":"echo x","label":"bad3"})");
    REQUIRE_FALSE(r3.success);
    REQUIRE(r3.output.find("Invalid cron schedule") != std::string::npos);

    // Non-numeric junk
    auto r4 = tool.execute(R"({"action":"add","schedule":"abc def ghi jkl mno","command":"echo x","label":"bad4"})");
    REQUIRE_FALSE(r4.success);
    REQUIRE(r4.output.find("Invalid cron schedule") != std::string::npos);
}

// ═══ CronTool: list ══════════════════════════════════════════════

TEST_CASE("CronTool: list succeeds", "[tools][cron]") {
    CronTool tool;
    auto result = tool.execute(R"({"action":"list"})");
    REQUIRE(result.success);
}

// ═══ CronTool: remove nonexistent ════════════════════════════════

TEST_CASE("CronTool: remove nonexistent label", "[tools][cron]") {
    CronTool tool;
    auto result = tool.execute(R"({"action":"remove","label":"__ptrclaw_nonexistent_label_xyz"})");
    REQUIRE_FALSE(result.success);
    REQUIRE(result.output.find("No ptrclaw entry") != std::string::npos);
}

// ═══ CronTool: add + remove roundtrip ════════════════════════════
// Tagged [cron-live] — may be skipped in CI where crontab is unavailable

TEST_CASE("CronTool: add and remove roundtrip", "[tools][cron][cron-live]") {
    CronTool tool;
    const std::string label = "__ptrclaw_test_roundtrip";

    // Clean up in case previous run left it
    tool.execute(R"({"action":"remove","label":")" + label + R"("})");

    // Add
    auto add_result = tool.execute(
        R"({"action":"add","schedule":"0 3 * * *","command":"echo roundtrip_test","label":")" + label + R"("})");

    if (!add_result.success) {
        // crontab may not be available (e.g. CI) — skip gracefully
        WARN("Skipping roundtrip test: crontab not available (" + add_result.output + ")");
        return;
    }
    REQUIRE(add_result.success);

    // Verify it appears in list
    auto list_result = tool.execute(R"({"action":"list"})");
    REQUIRE(list_result.success);
    REQUIRE(list_result.output.find("ptrclaw:" + label) != std::string::npos);
    REQUIRE(list_result.output.find("echo roundtrip_test") != std::string::npos);

    // Duplicate add should fail
    auto dup_result = tool.execute(
        R"({"action":"add","schedule":"0 3 * * *","command":"echo dup","label":")" + label + R"("})");
    REQUIRE_FALSE(dup_result.success);
    REQUIRE(dup_result.output.find("already exists") != std::string::npos);

    // Remove
    auto remove_result = tool.execute(R"({"action":"remove","label":")" + label + R"("})");
    REQUIRE(remove_result.success);

    // Verify it's gone
    auto list2 = tool.execute(R"({"action":"list"})");
    REQUIRE(list2.success);
    REQUIRE(list2.output.find("ptrclaw:" + label) == std::string::npos);
}
