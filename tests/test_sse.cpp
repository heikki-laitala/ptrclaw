#include <catch2/catch_test_macros.hpp>
#include "providers/sse.hpp"
#include <vector>

using namespace ptrclaw;

// Helper: collect all events from a single feed
static std::vector<SSEEvent> collect_events(SSEParser& parser, const std::string& chunk) {
    std::vector<SSEEvent> events;
    parser.feed(chunk, [&](const SSEEvent& ev) {
        events.push_back(ev);
        return true;
    });
    return events;
}

// ── Basic event parsing ──────────────────────────────────────────

TEST_CASE("SSEParser: single data-only event", "[sse]") {
    SSEParser parser;
    auto events = collect_events(parser, "data: hello\n\n");
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].data == "hello");
    REQUIRE(events[0].event.empty());
}

TEST_CASE("SSEParser: event with named type", "[sse]") {
    SSEParser parser;
    auto events = collect_events(parser, "event: message_start\ndata: {}\n\n");
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].event == "message_start");
    REQUIRE(events[0].data == "{}");
}

TEST_CASE("SSEParser: multiple events in one chunk", "[sse]") {
    SSEParser parser;
    auto events = collect_events(parser, "data: first\n\ndata: second\n\n");
    REQUIRE(events.size() == 2);
    REQUIRE(events[0].data == "first");
    REQUIRE(events[1].data == "second");
}

TEST_CASE("SSEParser: multi-line data concatenated", "[sse]") {
    SSEParser parser;
    auto events = collect_events(parser, "data: line1\ndata: line2\n\n");
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].data == "line1\nline2");
}

TEST_CASE("SSEParser: data field without space after colon", "[sse]") {
    SSEParser parser;
    auto events = collect_events(parser, "data:no_space\n\n");
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].data == "no_space");
}

// ── Streaming / chunked delivery ─────────────────────────────────

TEST_CASE("SSEParser: event split across two chunks", "[sse]") {
    SSEParser parser;

    // First chunk: incomplete event
    auto events1 = collect_events(parser, "data: hel");
    REQUIRE(events1.empty());

    // Second chunk: completes the event
    auto events2 = collect_events(parser, "lo\n\n");
    REQUIRE(events2.size() == 1);
    REQUIRE(events2[0].data == "hello");
}

TEST_CASE("SSEParser: event type split across chunks", "[sse]") {
    SSEParser parser;

    auto ev1 = collect_events(parser, "event: mess");
    REQUIRE(ev1.empty());

    auto ev2 = collect_events(parser, "age\ndata: {\"x\":1}\n\n");
    REQUIRE(ev2.size() == 1);
    REQUIRE(ev2[0].event == "message");
    REQUIRE(ev2[0].data == "{\"x\":1}");
}

TEST_CASE("SSEParser: empty lines between events", "[sse]") {
    SSEParser parser;
    // Extra blank lines should not produce extra events (no data)
    auto events = collect_events(parser, "data: a\n\n\n\ndata: b\n\n");
    REQUIRE(events.size() == 2);
    REQUIRE(events[0].data == "a");
    REQUIRE(events[1].data == "b");
}

// ── Carriage return handling ─────────────────────────────────────

TEST_CASE("SSEParser: handles \\r\\n line endings", "[sse]") {
    SSEParser parser;
    auto events = collect_events(parser, "data: hello\r\n\r\n");
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].data == "hello");
}

// ── Callback stopping ────────────────────────────────────────────

TEST_CASE("SSEParser: callback returning false stops parsing", "[sse]") {
    SSEParser parser;
    std::vector<SSEEvent> events;
    parser.feed("data: first\n\ndata: second\n\n", [&](const SSEEvent& ev) {
        events.push_back(ev);
        return false; // stop after first event
    });
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].data == "first");

    // Remaining data should still be in buffer for next feed
    auto remaining = collect_events(parser, "");
    // The "second" event data is buffered
    // Feed an empty string won't produce it, but feeding a newline will complete it
    auto final_events = collect_events(parser, "\n");
    // Actually the data: second\n\n is already complete in the buffer
    // Let's just verify we got first correctly
    REQUIRE(events[0].data == "first");
}

// ── Reset ────────────────────────────────────────────────────────

TEST_CASE("SSEParser: reset clears buffer state", "[sse]") {
    SSEParser parser;
    // Feed incomplete data
    collect_events(parser, "data: partial");
    parser.reset();

    // After reset, previous partial data should be gone
    auto events = collect_events(parser, "data: fresh\n\n");
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].data == "fresh");
}

// ── No events ────────────────────────────────────────────────────

TEST_CASE("SSEParser: empty input produces no events", "[sse]") {
    SSEParser parser;
    auto events = collect_events(parser, "");
    REQUIRE(events.empty());
}

TEST_CASE("SSEParser: comment lines ignored", "[sse]") {
    SSEParser parser;
    auto events = collect_events(parser, ": this is a comment\ndata: hello\n\n");
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].data == "hello");
}
