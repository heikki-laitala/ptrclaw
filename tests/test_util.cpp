#include <catch2/catch_test_macros.hpp>
#include "util.hpp"

using namespace ptrclaw;

// ── json_escape ──────────────────────────────────────────────────

TEST_CASE("json_escape: plain string unchanged", "[util]") {
    REQUIRE(json_escape("hello world") == "hello world");
}

TEST_CASE("json_escape: escapes backslash", "[util]") {
    REQUIRE(json_escape("a\\b") == "a\\\\b");
}

TEST_CASE("json_escape: escapes double quote", "[util]") {
    REQUIRE(json_escape("say \"hi\"") == "say \\\"hi\\\"");
}

TEST_CASE("json_escape: escapes newline, carriage return, tab", "[util]") {
    REQUIRE(json_escape("a\nb") == "a\\nb");
    REQUIRE(json_escape("a\rb") == "a\\rb");
    REQUIRE(json_escape("a\tb") == "a\\tb");
}

TEST_CASE("json_escape: escapes control characters as unicode", "[util]") {
    std::string input(1, '\x01');
    REQUIRE(json_escape(input) == "\\u0001");
}

TEST_CASE("json_escape: empty string", "[util]") {
    REQUIRE(json_escape("").empty());
}

// ── json_unescape ────────────────────────────────────────────────

TEST_CASE("json_unescape: plain string unchanged", "[util]") {
    REQUIRE(json_unescape("hello world") == "hello world");
}

TEST_CASE("json_unescape: unescapes backslash", "[util]") {
    REQUIRE(json_unescape("a\\\\b") == "a\\b");
}

TEST_CASE("json_unescape: unescapes double quote", "[util]") {
    REQUIRE(json_unescape("say \\\"hi\\\"") == "say \"hi\"");
}

TEST_CASE("json_unescape: unescapes newline, carriage return, tab", "[util]") {
    REQUIRE(json_unescape("a\\nb") == "a\nb");
    REQUIRE(json_unescape("a\\rb") == "a\rb");
    REQUIRE(json_unescape("a\\tb") == "a\tb");
}

TEST_CASE("json_unescape: unescapes unicode BMP codepoint", "[util]") {
    // \u0041 == 'A'
    REQUIRE(json_unescape("\\u0041") == "A");
    // \u00e9 == 'é' (U+00E9, 2-byte UTF-8)
    REQUIRE(json_unescape("\\u00e9") == "\xc3\xa9");
    // \u4e16 == '世' (U+4E16, 3-byte UTF-8)
    REQUIRE(json_unescape("\\u4e16") == "\xe4\xb8\x96");
}

TEST_CASE("json_unescape: roundtrip with json_escape", "[util]") {
    std::string original = "line1\nline2\ttab \"quoted\" back\\slash";
    REQUIRE(json_unescape(json_escape(original)) == original);
}

TEST_CASE("json_unescape: empty string", "[util]") {
    REQUIRE(json_unescape("").empty());
}

// ── trim ─────────────────────────────────────────────────────────

TEST_CASE("trim: removes leading and trailing spaces", "[util]") {
    REQUIRE(trim("  hello  ") == "hello");
}

TEST_CASE("trim: removes tabs and mixed whitespace", "[util]") {
    REQUIRE(trim("\t hello \n") == "hello");
}

TEST_CASE("trim: empty string returns empty", "[util]") {
    REQUIRE(trim("").empty());
}

TEST_CASE("trim: all whitespace returns empty", "[util]") {
    REQUIRE(trim("   \t\n  ").empty());
}

TEST_CASE("trim: no whitespace unchanged", "[util]") {
    REQUIRE(trim("hello") == "hello");
}

// ── split ────────────────────────────────────────────────────────

TEST_CASE("split: normal delimiter", "[util]") {
    auto parts = split("a,b,c", ',');
    REQUIRE(parts == std::vector<std::string>{"a", "b", "c"});
}

TEST_CASE("split: empty parts preserved", "[util]") {
    auto parts = split("a,,b", ',');
    REQUIRE(parts == std::vector<std::string>{"a", "", "b"});
}

TEST_CASE("split: no delimiter found", "[util]") {
    auto parts = split("hello", ',');
    REQUIRE(parts == std::vector<std::string>{"hello"});
}

TEST_CASE("split: empty string", "[util]") {
    auto parts = split("", ',');
    REQUIRE(parts.empty());
}

// ── replace_all ──────────────────────────────────────────────────

TEST_CASE("replace_all: single replacement", "[util]") {
    REQUIRE(replace_all("hello world", "world", "there") == "hello there");
}

TEST_CASE("replace_all: multiple replacements", "[util]") {
    REQUIRE(replace_all("aaa", "a", "bb") == "bbbbbb");
}

TEST_CASE("replace_all: no match", "[util]") {
    REQUIRE(replace_all("hello", "xyz", "abc") == "hello");
}

TEST_CASE("replace_all: empty from returns original", "[util]") {
    REQUIRE(replace_all("hello", "", "abc") == "hello");
}

TEST_CASE("replace_all: replace with empty", "[util]") {
    REQUIRE(replace_all("hello", "l", "") == "heo");
}

// ── estimate_tokens ──────────────────────────────────────────────

TEST_CASE("estimate_tokens: empty string is zero", "[util]") {
    REQUIRE(estimate_tokens("") == 0);
}

TEST_CASE("estimate_tokens: known lengths", "[util]") {
    // 4 chars per token
    REQUIRE(estimate_tokens("abcd") == 1);
    REQUIRE(estimate_tokens("abcdefgh") == 2);
    REQUIRE(estimate_tokens("ab") == 0); // integer division
}

// ── expand_home ──────────────────────────────────────────────────

TEST_CASE("expand_home: path without tilde unchanged", "[util]") {
    REQUIRE(expand_home("/usr/local") == "/usr/local");
}

TEST_CASE("expand_home: tilde is expanded", "[util]") {
    std::string result = expand_home("~/Documents");
    REQUIRE(result.front() == '/');
    REQUIRE(result.find('~') == std::string::npos);
    REQUIRE(result.size() > std::string("/Documents").size());
}

TEST_CASE("expand_home: empty string unchanged", "[util]") {
    REQUIRE(expand_home("").empty());
}
