#include <catch2/catch_test_macros.hpp>
#include "event_bus.hpp"

using namespace ptrclaw;

// ── Basic publish / subscribe ───────────────────────────────────

TEST_CASE("EventBus: subscribe and publish", "[event_bus]") {
    EventBus bus;
    int count = 0;

    bus.subscribe(MessageReceivedEvent::TAG, [&](const Event&) {
        count++;
    });

    MessageReceivedEvent ev;
    ev.session_id = "s1";
    bus.publish(ev);

    REQUIRE(count == 1);
}

TEST_CASE("EventBus: multiple subscribers called in order", "[event_bus]") {
    EventBus bus;
    std::vector<int> order;

    bus.subscribe(MessageReadyEvent::TAG, [&](const Event&) {
        order.push_back(1);
    });
    bus.subscribe(MessageReadyEvent::TAG, [&](const Event&) {
        order.push_back(2);
    });

    MessageReadyEvent ev;
    bus.publish(ev);

    REQUIRE(order.size() == 2);
    REQUIRE(order[0] == 1);
    REQUIRE(order[1] == 2);
}

TEST_CASE("EventBus: publish with no subscribers is a no-op", "[event_bus]") {
    EventBus bus;
    MessageReceivedEvent ev;
    bus.publish(ev); // should not crash
}

TEST_CASE("EventBus: different tags are independent", "[event_bus]") {
    EventBus bus;
    int received_count = 0;
    int ready_count = 0;

    bus.subscribe(MessageReceivedEvent::TAG, [&](const Event&) {
        received_count++;
    });
    bus.subscribe(MessageReadyEvent::TAG, [&](const Event&) {
        ready_count++;
    });

    MessageReceivedEvent ev1;
    bus.publish(ev1);
    bus.publish(ev1);

    MessageReadyEvent ev2;
    bus.publish(ev2);

    REQUIRE(received_count == 2);
    REQUIRE(ready_count == 1);
}

// ── Unsubscribe ─────────────────────────────────────────────────

TEST_CASE("EventBus: unsubscribe removes handler", "[event_bus]") {
    EventBus bus;
    int count = 0;

    uint64_t id = bus.subscribe(MessageReceivedEvent::TAG, [&](const Event&) {
        count++;
    });

    MessageReceivedEvent ev;
    bus.publish(ev);
    REQUIRE(count == 1);

    REQUIRE(bus.unsubscribe(id));
    bus.publish(ev);
    REQUIRE(count == 1); // not called again
}

TEST_CASE("EventBus: unsubscribe returns false for unknown id", "[event_bus]") {
    EventBus bus;
    REQUIRE_FALSE(bus.unsubscribe(999));
}

// ── Clear ───────────────────────────────────────────────────────

TEST_CASE("EventBus: clear removes all subscriptions", "[event_bus]") {
    EventBus bus;
    int count = 0;

    bus.subscribe(MessageReceivedEvent::TAG, [&](const Event&) { count++; });
    bus.subscribe(MessageReadyEvent::TAG, [&](const Event&) { count++; });

    bus.clear();

    MessageReceivedEvent ev1;
    MessageReadyEvent ev2;
    bus.publish(ev1);
    bus.publish(ev2);
    REQUIRE(count == 0);
}

// ── subscriber_count ────────────────────────────────────────────

TEST_CASE("EventBus: subscriber_count", "[event_bus]") {
    EventBus bus;
    REQUIRE(bus.subscriber_count(MessageReceivedEvent::TAG) == 0);

    bus.subscribe(MessageReceivedEvent::TAG, [](const Event&) {});
    bus.subscribe(MessageReceivedEvent::TAG, [](const Event&) {});
    REQUIRE(bus.subscriber_count(MessageReceivedEvent::TAG) == 2);
    REQUIRE(bus.subscriber_count(MessageReadyEvent::TAG) == 0);
}

// ── Type-safe subscribe helper ──────────────────────────────────

TEST_CASE("EventBus: type-safe subscribe template", "[event_bus]") {
    EventBus bus;
    std::string captured_session;

    subscribe<MessageReceivedEvent>(bus, [&](const MessageReceivedEvent& ev) {
        captured_session = ev.session_id;
    });

    MessageReceivedEvent ev;
    ev.session_id = "test-session";
    bus.publish(ev);

    REQUIRE(captured_session == "test-session");
}

TEST_CASE("EventBus: type-safe subscribe for ProviderResponse", "[event_bus]") {
    EventBus bus;
    bool captured_has_tools = false;

    subscribe<ProviderResponseEvent>(bus, [&](const ProviderResponseEvent& ev) {
        captured_has_tools = ev.has_tool_calls;
    });

    ProviderResponseEvent ev;
    ev.has_tool_calls = true;
    bus.publish(ev);

    REQUIRE(captured_has_tools);
}

// ── Event data integrity ────────────────────────────────────────

TEST_CASE("EventBus: event data passes through correctly", "[event_bus]") {
    EventBus bus;
    std::string tool_name;
    bool success = false;

    subscribe<ToolCallResultEvent>(bus, [&](const ToolCallResultEvent& ev) {
        tool_name = ev.tool_name;
        success = ev.success;
    });

    ToolCallResultEvent ev;
    ev.session_id = "s1";
    ev.tool_name = "shell";
    ev.success = true;
    bus.publish(ev);

    REQUIRE(tool_name == "shell");
    REQUIRE(success);
}

TEST_CASE("EventBus: session events carry session_id", "[event_bus]") {
    EventBus bus;
    std::string created_id;
    std::string evicted_id;

    subscribe<SessionCreatedEvent>(bus, [&](const SessionCreatedEvent& ev) {
        created_id = ev.session_id;
    });
    subscribe<SessionEvictedEvent>(bus, [&](const SessionEvictedEvent& ev) {
        evicted_id = ev.session_id;
    });

    SessionCreatedEvent e1;
    e1.session_id = "abc";
    bus.publish(e1);

    SessionEvictedEvent e2;
    e2.session_id = "xyz";
    bus.publish(e2);

    REQUIRE(created_id == "abc");
    REQUIRE(evicted_id == "xyz");
}
