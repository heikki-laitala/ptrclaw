#include <catch2/catch_test_macros.hpp>
#include "channels/embed.hpp"
#include "test_helpers.hpp"
#include "config.hpp"
#include "event.hpp"
#include "event_bus.hpp"
#include "session.hpp"
#include "stream_relay.hpp"
#include "http.hpp"
#include <atomic>
#include <thread>

using namespace ptrclaw;

// ── EmbedChannel unit tests ─────────────────────────────────────

TEST_CASE("EmbedChannel: channel_name is embed", "[embed]") {
    EmbedChannel ch;
    REQUIRE(ch.channel_name() == "embed");
    REQUIRE(ch.health_check());
    REQUIRE(ch.supports_polling());
}

TEST_CASE("EmbedChannel: poll_updates returns empty when no messages", "[embed]") {
    EmbedChannel ch;
    auto msgs = ch.poll_updates();
    REQUIRE(msgs.empty());
}

TEST_CASE("EmbedChannel: send_message delivers to pending response", "[embed]") {
    EmbedChannel ch;

    // Simulate what SessionManager does: call send_message with session_id as target.
    // First we need a pending response slot — normally send_user_message creates it,
    // but we can test the channel interface directly via a background thread.
    std::string response;
    std::thread sender([&] {
        response = ch.send_user_message("test-session", "hello");
    });

    // Poll the message out (give sender thread time to push)
    std::vector<ChannelMessage> msgs;
    for (int i = 0; i < 50 && msgs.empty(); ++i) {
        msgs = ch.poll_updates();
        if (msgs.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    REQUIRE(msgs.size() == 1);
    REQUIRE(msgs[0].sender == "test-session");
    REQUIRE(msgs[0].content == "hello");
    REQUIRE(msgs[0].channel == "embed");
    REQUIRE(msgs[0].reply_target.value_or("") == "test-session");

    // Deliver a response (simulating what StreamRelay/SessionManager does)
    ch.send_message("test-session", "world");

    sender.join();
    REQUIRE(response == "world");
}

TEST_CASE("EmbedChannel: stream callback lifecycle", "[embed]") {
    EmbedChannel ch;

    REQUIRE(ch.get_stream_callback("s1") == nullptr);

    std::vector<std::string> chunks;
    ch.set_stream_callback("s1", [&](const char* chunk, int /*done*/) {
        chunks.emplace_back(chunk);
    });

    auto cb = ch.get_stream_callback("s1");
    REQUIRE(cb != nullptr);
    cb("hello", 0);
    REQUIRE(chunks.size() == 1);
    REQUIRE(chunks[0] == "hello");

    ch.clear_stream_callback("s1");
    REQUIRE(ch.get_stream_callback("s1") == nullptr);
}

// ── C API tests ─────────────────────────────────────────────────

#include "../../include/ptrclaw/ptrclaw.h"

TEST_CASE("ptrclaw_version: returns non-empty string", "[embed]") {
    const char* ver = ptrclaw_version();
    REQUIRE(ver != nullptr);
    REQUIRE(std::string(ver).find('.') != std::string::npos);
}

TEST_CASE("ptrclaw C API: null handle returns errors", "[embed]") {
    REQUIRE(ptrclaw_send(nullptr, "s", "m") == PTRCLAW_ERR_INVALID);
    REQUIRE(std::string(ptrclaw_last_error(nullptr)) == "null handle");
    REQUIRE(std::string(ptrclaw_last_response(nullptr, "s")).empty());
}

TEST_CASE("ptrclaw C API: null args return errors", "[embed]") {
    // We can't easily create a real handle without network access,
    // so just test the null-guard paths.
    REQUIRE(ptrclaw_send(nullptr, nullptr, "m") == PTRCLAW_ERR_INVALID);
    REQUIRE(ptrclaw_send(nullptr, "s", nullptr) == PTRCLAW_ERR_INVALID);
    REQUIRE(ptrclaw_send_stream(nullptr, "s", "m", nullptr, nullptr)
            == PTRCLAW_ERR_INVALID);
}
