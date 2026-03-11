#include <catch2/catch_test_macros.hpp>
#include "output_filter.hpp"

using namespace ptrclaw;

// ── strip_ansi_codes ────────────────────────────────────────────

TEST_CASE("strip_ansi_codes: removes color codes", "[output_filter]") {
    REQUIRE(strip_ansi_codes("\033[31mred\033[0m") == "red");
}

TEST_CASE("strip_ansi_codes: preserves plain text", "[output_filter]") {
    REQUIRE(strip_ansi_codes("hello world") == "hello world");
}

TEST_CASE("strip_ansi_codes: handles empty string", "[output_filter]") {
    REQUIRE(strip_ansi_codes("").empty());
}

TEST_CASE("strip_ansi_codes: removes bold and underline", "[output_filter]") {
    REQUIRE(strip_ansi_codes("\033[1mbold\033[0m \033[4munderline\033[0m") == "bold underline");
}

TEST_CASE("strip_ansi_codes: removes 256-color codes", "[output_filter]") {
    REQUIRE(strip_ansi_codes("\033[38;5;196mred\033[0m") == "red");
}

// ── filter_tool_output: basic ───────────────────────────────────

TEST_CASE("filter_tool_output: passes short output through", "[output_filter]") {
    REQUIRE(filter_tool_output("hello") == "hello");
}

TEST_CASE("filter_tool_output: empty input", "[output_filter]") {
    REQUIRE(filter_tool_output("").empty());
}

TEST_CASE("filter_tool_output: strips ANSI by default", "[output_filter]") {
    std::string result = filter_tool_output("\033[32mOK\033[0m");
    REQUIRE(result == "OK");
}

// ── filter_tool_output: line truncation ─────────────────────────

TEST_CASE("filter_tool_output: truncates long lines", "[output_filter]") {
    OutputFilterConfig config;
    config.max_line_length = 10;
    std::string input = "short\nabcdefghijklmnop\nend";
    std::string result = filter_tool_output(input, config);
    REQUIRE(result.find("abcdefghij...") != std::string::npos);
    REQUIRE(result.find("short") != std::string::npos);
    REQUIRE(result.find("end") != std::string::npos);
}

// ── filter_tool_output: max lines ───────────────────────────────

TEST_CASE("filter_tool_output: caps output at max_lines", "[output_filter]") {
    OutputFilterConfig config;
    config.max_lines = 3;
    std::string input = "line1\nline2\nline3\nline4\nline5\n";
    std::string result = filter_tool_output(input, config);
    REQUIRE(result.find("line1") != std::string::npos);
    REQUIRE(result.find("line3") != std::string::npos);
    REQUIRE(result.find("line4") == std::string::npos);
    REQUIRE(result.find("truncated") != std::string::npos);
}

// ── filter_tool_output: collapse blank lines ────────────────────

TEST_CASE("filter_tool_output: collapses consecutive blank lines", "[output_filter]") {
    std::string input = "a\n\n\n\nb";
    std::string result = filter_tool_output(input);
    // Should have at most one blank line between a and b
    REQUIRE(result.find("a\n\nb") != std::string::npos);
    REQUIRE(result.find("a\n\n\nb") == std::string::npos);
}

// ── filter_tool_output: max total chars ─────────────────────────

TEST_CASE("filter_tool_output: respects max_total_chars", "[output_filter]") {
    OutputFilterConfig config;
    config.max_total_chars = 20;
    config.max_lines = 1000; // high limit so chars is the binding constraint
    std::string input;
    for (int i = 0; i < 100; i++) {
        input += "line " + std::to_string(i) + "\n";
    }
    std::string result = filter_tool_output(input, config);
    REQUIRE(result.size() < 100); // well under the full input
    REQUIRE(result.find("truncated") != std::string::npos);
}

// ── filter_tool_output: config overrides ────────────────────────

TEST_CASE("filter_tool_output: strip_ansi=false preserves codes", "[output_filter]") {
    OutputFilterConfig config;
    config.strip_ansi = false;
    std::string input = "\033[31mred\033[0m";
    REQUIRE(filter_tool_output(input, config) == input);
}

TEST_CASE("filter_tool_output: collapse_blank_lines=false keeps all blanks", "[output_filter]") {
    OutputFilterConfig config;
    config.collapse_blank_lines = false;
    std::string input = "a\n\n\nb";
    std::string result = filter_tool_output(input, config);
    REQUIRE(result == input);
}
