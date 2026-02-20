#include <catch2/catch_test_macros.hpp>
#include "channel.hpp"

using namespace ptrclaw;

// ── split_message ────────────────────────────────────────────────

TEST_CASE("Channel::split_message: short message returns single part", "[channel]") {
    auto parts = Channel::split_message("Hello world", 100);
    REQUIRE(parts.size() == 1);
    REQUIRE(parts[0] == "Hello world");
}

TEST_CASE("Channel::split_message: empty message returns empty", "[channel]") {
    auto parts = Channel::split_message("", 100);
    REQUIRE(parts.empty());
}

TEST_CASE("Channel::split_message: splits at newline boundary", "[channel]") {
    std::string text = "Line one\nLine two\nLine three";
    auto parts = Channel::split_message(text, 15);
    REQUIRE(parts.size() >= 2);
    // First part should end at a newline boundary
    REQUIRE(parts[0].find('\n') != std::string::npos);
}

TEST_CASE("Channel::split_message: splits at space when no newline", "[channel]") {
    std::string text = "word1 word2 word3 word4 word5";
    auto parts = Channel::split_message(text, 12);
    REQUIRE(parts.size() >= 2);
    // Verify no part exceeds max_len
    for (const auto& p : parts) {
        REQUIRE(p.size() <= 12);
    }
}

TEST_CASE("Channel::split_message: hard cut when no space or newline", "[channel]") {
    std::string text = "abcdefghijklmnop";
    auto parts = Channel::split_message(text, 5);
    REQUIRE(parts.size() >= 3);
    REQUIRE(parts[0].size() == 5);
}

TEST_CASE("Channel::split_message: exact fit returns single part", "[channel]") {
    std::string text = "exact";
    auto parts = Channel::split_message(text, 5);
    REQUIRE(parts.size() == 1);
    REQUIRE(parts[0] == "exact");
}

TEST_CASE("Channel::split_message: zero max_len returns empty", "[channel]") {
    auto parts = Channel::split_message("hello", 0);
    REQUIRE(parts.empty());
}

TEST_CASE("Channel::split_message: preserves all content", "[channel]") {
    std::string text = "Hello world, this is a longer message that should be split into parts.";
    auto parts = Channel::split_message(text, 20);
    std::string reassembled;
    for (const auto& p : parts) reassembled += p;
    REQUIRE(reassembled == text);
}
