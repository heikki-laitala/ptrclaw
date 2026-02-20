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

// ── ChannelRegistry ──────────────────────────────────────────────

namespace {
class DummyChannel : public Channel {
public:
    explicit DummyChannel(std::string name) : name_(std::move(name)) {}
    std::string channel_name() const override { return name_; }
    bool health_check() override { return true; }
    void send_message(const std::string& /*target*/, const std::string& /*msg*/) override {}
private:
    std::string name_;
};
}

TEST_CASE("ChannelRegistry: starts empty", "[channel]") {
    ChannelRegistry reg;
    REQUIRE(reg.size() == 0);
    REQUIRE(reg.channel_names().empty());
}

TEST_CASE("ChannelRegistry: register and find", "[channel]") {
    ChannelRegistry reg;
    reg.register_channel(std::make_unique<DummyChannel>("test"));
    REQUIRE(reg.size() == 1);
    REQUIRE(reg.find_by_name("test") != nullptr);
    REQUIRE(reg.find_by_name("test")->channel_name() == "test");
}

TEST_CASE("ChannelRegistry: find_by_name returns null for unknown", "[channel]") {
    ChannelRegistry reg;
    reg.register_channel(std::make_unique<DummyChannel>("test"));
    REQUIRE(reg.find_by_name("other") == nullptr);
}

TEST_CASE("ChannelRegistry: multiple channels", "[channel]") {
    ChannelRegistry reg;
    reg.register_channel(std::make_unique<DummyChannel>("a"));
    reg.register_channel(std::make_unique<DummyChannel>("b"));
    reg.register_channel(std::make_unique<DummyChannel>("c"));
    REQUIRE(reg.size() == 3);
    auto names = reg.channel_names();
    REQUIRE(names.size() == 3);
    REQUIRE(reg.find_by_name("b") != nullptr);
}
