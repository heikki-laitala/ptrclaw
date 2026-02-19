#include <catch2/catch_test_macros.hpp>
#include "dispatcher.hpp"

using namespace ptrclaw;

// ── repair_json ──────────────────────────────────────────────────

TEST_CASE("repair_json: valid JSON passes through", "[dispatcher]") {
    std::string input = R"({"name":"foo","value":42})";
    REQUIRE(repair_json(input) == input);
}

TEST_CASE("repair_json: missing closing brace", "[dispatcher]") {
    std::string input = R"({"name":"foo")";
    std::string result = repair_json(input);
    // Should be parseable JSON after repair
    REQUIRE(result.back() == '}');
    REQUIRE(result.find("foo") != std::string::npos);
}

TEST_CASE("repair_json: trailing comma removed", "[dispatcher]") {
    std::string input = R"({"a":1,"b":2,})";
    std::string result = repair_json(input);
    REQUIRE(result.find(",}") == std::string::npos);
}

TEST_CASE("repair_json: trailing comma with whitespace", "[dispatcher]") {
    std::string input = "{\"a\":1, }";
    std::string result = repair_json(input);
    REQUIRE(result.find(",") == std::string::npos);
}

TEST_CASE("repair_json: missing brace and trailing comma combined", "[dispatcher]") {
    std::string input = R"({"a":1,"b":2,)";
    std::string result = repair_json(input);
    REQUIRE(result.back() == '}');
    REQUIRE(result.find(",}") == std::string::npos);
}

// ── parse_xml_tool_calls ─────────────────────────────────────────

TEST_CASE("parse_xml_tool_calls: single tool call", "[dispatcher]") {
    std::string text = R"(Some text
<tool_call>
{"name":"read_file","arguments":{"path":"/tmp/a.txt"}}
</tool_call>
More text)";

    auto calls = parse_xml_tool_calls(text);
    REQUIRE(calls.size() == 1);
    REQUIRE(calls[0].name == "read_file");
    REQUIRE(calls[0].arguments.find("/tmp/a.txt") != std::string::npos);
    REQUIRE_FALSE(calls[0].id.empty());
}

TEST_CASE("parse_xml_tool_calls: multiple tool calls", "[dispatcher]") {
    std::string text = R"(
<tool_call>
{"name":"tool_a","arguments":{}}
</tool_call>
<tool_call>
{"name":"tool_b","arguments":{"x":1}}
</tool_call>
)";

    auto calls = parse_xml_tool_calls(text);
    REQUIRE(calls.size() == 2);
    REQUIRE(calls[0].name == "tool_a");
    REQUIRE(calls[1].name == "tool_b");
}

TEST_CASE("parse_xml_tool_calls: no tool calls", "[dispatcher]") {
    auto calls = parse_xml_tool_calls("Just some regular text.");
    REQUIRE(calls.empty());
}

TEST_CASE("parse_xml_tool_calls: malformed JSON skipped", "[dispatcher]") {
    std::string text = R"(
<tool_call>
this is not json at all {{{
</tool_call>
<tool_call>
{"name":"valid","arguments":{}}
</tool_call>
)";

    auto calls = parse_xml_tool_calls(text);
    REQUIRE(calls.size() == 1);
    REQUIRE(calls[0].name == "valid");
}

TEST_CASE("parse_xml_tool_calls: missing name skipped", "[dispatcher]") {
    std::string text = R"(
<tool_call>
{"arguments":{"x":1}}
</tool_call>
)";

    auto calls = parse_xml_tool_calls(text);
    REQUIRE(calls.empty());
}

// ── format_tool_results_xml ──────────────────────────────────────

TEST_CASE("format_tool_results_xml: success", "[dispatcher]") {
    std::string result = format_tool_results_xml("read_file", true, "file contents");
    REQUIRE(result == R"(<tool_result name="read_file" status="ok">file contents</tool_result>)");
}

TEST_CASE("format_tool_results_xml: failure", "[dispatcher]") {
    std::string result = format_tool_results_xml("write_file", false, "permission denied");
    REQUIRE(result == R"(<tool_result name="write_file" status="error">permission denied</tool_result>)");
}

// ── format_tool_result_message ───────────────────────────────────

TEST_CASE("format_tool_result_message: success message", "[dispatcher]") {
    auto msg = format_tool_result_message("call_123", "read_file", true, "file contents");
    REQUIRE(msg.role == Role::Tool);
    REQUIRE(msg.content == "file contents");
    REQUIRE(msg.name.value() == "read_file");
    REQUIRE(msg.tool_call_id.value() == "call_123");
}

TEST_CASE("format_tool_result_message: error message", "[dispatcher]") {
    auto msg = format_tool_result_message("call_456", "shell", false, "command failed");
    REQUIRE(msg.role == Role::Tool);
    REQUIRE(msg.content == "Error: command failed");
    REQUIRE(msg.name.value() == "shell");
    REQUIRE(msg.tool_call_id.value() == "call_456");
}
