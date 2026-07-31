#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "channels/http.hpp"
#include "event.hpp"
#include "event_bus.hpp"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace ptrclaw;
using json = nlohmann::json;

namespace {

HttpChannelConfig test_config() {
    HttpChannelConfig cfg;
    cfg.listen = "127.0.0.1:18099";
    cfg.turn_timeout_seconds = 5;
    return cfg;
}

WebhookRequest chat_request(const json& body) {
    WebhookRequest req;
    req.method = "POST";
    req.path = "/chat";
    req.body = body.dump();
    return req;
}

} // namespace

// ── Routing, authentication, validation ─────────────────────────────
//
// Driven through handle_request() rather than a socket: these are decisions about a parsed
// request, and a TCP connection would add nothing but flakiness. The streaming half below
// does need the cross-thread machinery, and gets it.

TEST_CASE("HttpChannel: /healthz answers without authentication", "[http_channel]") {
    // Container probes cannot carry the shared secret, so this route must stay open even
    // when one is configured.
    auto cfg = test_config();
    cfg.secret = "s3cret";
    HttpChannel ch(cfg);

    WebhookRequest req;
    req.method = "GET";
    req.path = "/healthz";

    auto resp = ch.handle_request(req);
    REQUIRE(resp.status == 200);
    REQUIRE(resp.body == "ok");
}

TEST_CASE("HttpChannel: unknown path is 404 and wrong method is 405", "[http_channel]") {
    HttpChannel ch(test_config());

    WebhookRequest wrong_path;
    wrong_path.method = "POST";
    wrong_path.path = "/nope";
    REQUIRE(ch.handle_request(wrong_path).status == 404);

    WebhookRequest wrong_method;
    wrong_method.method = "GET";
    wrong_method.path = "/chat";
    REQUIRE(ch.handle_request(wrong_method).status == 405);
}

TEST_CASE("HttpChannel: a configured secret is required", "[http_channel]") {
    auto cfg = test_config();
    cfg.secret = "s3cret";
    HttpChannel ch(cfg);

    auto req = chat_request({{"session", "s1"}, {"message", "hi"}});
    REQUIRE(ch.handle_request(req).status == 403);  // no header at all

    req.headers["authorization"] = "Bearer wrong";
    REQUIRE(ch.handle_request(req).status == 403);

    req.headers["authorization"] = "Bearer s3cret";
    auto ok = ch.handle_request(req);
    REQUIRE(ok.status == 200);
    // The response must be a stream, not a body — the reply does not exist yet.
    REQUIRE(ok.content_type == "text/event-stream");
    REQUIRE(static_cast<bool>(ok.stream));
    REQUIRE(ok.body.empty());
}

TEST_CASE("HttpChannel: a rejected request queues nothing", "[http_channel]") {
    // The discriminating half of the auth test: a 403 that still enqueued the message
    // would run the turn anyway and simply refuse to show the answer.
    auto cfg = test_config();
    cfg.secret = "s3cret";
    HttpChannel ch(cfg);

    ch.handle_request(chat_request({{"session", "s1"}, {"message", "hi"}}));
    REQUIRE(ch.poll_updates().empty());
}

TEST_CASE("HttpChannel: malformed bodies are refused with 400", "[http_channel]") {
    HttpChannel ch(test_config());

    WebhookRequest bad_json;
    bad_json.method = "POST";
    bad_json.path = "/chat";
    bad_json.body = "{not json";
    REQUIRE(ch.handle_request(bad_json).status == 400);

    REQUIRE(ch.handle_request(chat_request({{"message", "hi"}})).status == 400);      // no session
    REQUIRE(ch.handle_request(chat_request({{"session", "s1"}})).status == 400);      // no message
    REQUIRE(ch.handle_request(chat_request({{"session", ""}, {"message", "hi"}})).status == 400);

    REQUIRE(ch.poll_updates().empty());
}

TEST_CASE("HttpChannel: a pushed history window reaches the queued message",
          "[http_channel]") {
    HttpChannel ch(test_config());

    auto resp = ch.handle_request(chat_request({
        {"session", "s1"},
        {"message", "and the second?"},
        {"history", json::array({
            json{{"role", "system"},    {"content", "You are terse."}},
            json{{"role", "user"},      {"content", "first question"}},
            json{{"role", "assistant"}, {"content", "first answer"}},
        })},
    }));
    REQUIRE(resp.status == 200);

    auto queued = ch.poll_updates();
    REQUIRE(queued.size() == 1);
    // sender and reply_target both matter and for different reasons: SessionManager keys
    // the session off sender, and send_message() routes the reply back by reply_target.
    REQUIRE(queued[0].sender == "s1");
    REQUIRE(queued[0].reply_target.value_or("") == "s1");
    REQUIRE(queued[0].channel == "http");
    REQUIRE(queued[0].content == "and the second?");

    const auto& maybe_window = queued[0].history;
    REQUIRE(maybe_window.has_value());
    // The redundant guard is for clang-tidy, which cannot see that a failed REQUIRE
    // aborts the test and so treats the access below as unchecked.
    if (!maybe_window.has_value()) return;
    const auto& window = *maybe_window;
    REQUIRE(window.size() == 3);
    REQUIRE(window[0].role == Role::System);
    REQUIRE(window[0].content == "You are terse.");
    REQUIRE(window[1].role == Role::User);
    REQUIRE(window[2].role == Role::Assistant);
    REQUIRE(window[2].content == "first answer");
}

TEST_CASE("HttpChannel: a malformed history is refused rather than dropped",
          "[http_channel]") {
    // Silently answering without the context the caller tried to supply is the worst
    // outcome: the reply looks fine and has forgotten the conversation.
    HttpChannel ch(test_config());

    REQUIRE(ch.handle_request(chat_request({
        {"session", "s1"}, {"message", "hi"}, {"history", "not-an-array"}})).status == 400);

    REQUIRE(ch.handle_request(chat_request({
        {"session", "s1"}, {"message", "hi"},
        {"history", json::array({json{{"role", "wizard"}, {"content", "x"}}})}})).status == 400);

    REQUIRE(ch.handle_request(chat_request({
        {"session", "s1"}, {"message", "hi"},
        {"history", json::array({json{{"role", "user"}}})}})).status == 400);

    REQUIRE(ch.poll_updates().empty());
}

TEST_CASE("HttpChannel: no history key means the agent keeps its own", "[http_channel]") {
    HttpChannel ch(test_config());
    ch.handle_request(chat_request({{"session", "s1"}, {"message", "hi"}}));

    auto queued = ch.poll_updates();
    REQUIRE(queued.size() == 1);
    REQUIRE_FALSE(queued[0].history.has_value());
}

TEST_CASE("HttpChannel: health_check rejects an unusable listen address", "[http_channel]") {
    auto cfg = test_config();
    cfg.listen = "not-a-host-port";
    HttpChannel ch(cfg);
    REQUIRE_FALSE(ch.health_check());
    REQUIRE(HttpChannel(test_config()).health_check());
}

// ── Streaming: the cross-thread path ────────────────────────────────
//
// Deltas are produced on the poll thread and written on a connection thread. Both halves
// are driven through their real public routes — a StreamChunkEvent on the bus (which is
// also the only thing that exercises set_event_bus) and send_message() for the reply — so
// this covers the wiring, not just the frame formatting.

TEST_CASE("HttpChannel: tokens stream in order and the turn ends with done",
          "[http_channel]") {
    EventBus bus;
    HttpChannel ch(test_config());
    ch.set_event_bus(&bus);

    auto resp = ch.handle_request(chat_request({{"session", "s1"}, {"message", "hi"}}));
    REQUIRE(static_cast<bool>(resp.stream));

    std::string written;
    std::atomic<bool> finished{false};
    std::thread consumer([&] {
        resp.stream([&](std::string_view chunk) {
            written.append(chunk);
            return true;
        });
        finished = true;
    });

    // Deltas arrive as the provider produces them...
    StreamChunkEvent a;
    a.session_id = "s1";
    a.delta = "Hel";
    bus.publish(a);

    StreamChunkEvent b;
    b.session_id = "s1";
    b.delta = "lo";
    bus.publish(b);

    // ...and MessageReadyEvent -> send_message() is what ends the turn. Deliberately not
    // StreamEndEvent: a non-streaming provider emits no stream events at all, so only
    // this path fires in that case.
    ch.send_message("s1", "Hello");

    consumer.join();
    REQUIRE(finished.load());

    REQUIRE(written.find("event: token") != std::string::npos);
    REQUIRE(written.find(R"({"delta":"Hel"})") != std::string::npos);
    REQUIRE(written.find(R"({"delta":"lo"})") != std::string::npos);
    REQUIRE(written.find(R"(event: done)") != std::string::npos);
    REQUIRE(written.find(R"({"content":"Hello"})") != std::string::npos);

    // Order is the property, not mere presence: a done frame ahead of a token would end
    // the stream early and the tokens would never be seen.
    REQUIRE(written.find("Hel") < written.find("lo"));
    REQUIRE(written.find("lo") < written.find("event: done"));
}

TEST_CASE("HttpChannel: a turn that never answers times out", "[http_channel]") {
    // Without this a provider that hangs holds the connection — and one of the process's
    // few connection slots — open indefinitely.
    auto cfg = test_config();
    cfg.turn_timeout_seconds = 1;
    HttpChannel ch(cfg);

    auto resp = ch.handle_request(chat_request({{"session", "s1"}, {"message", "hi"}}));

    std::string written;
    const auto started = std::chrono::steady_clock::now();
    resp.stream([&](std::string_view chunk) {
        written.append(chunk);
        return true;
    });
    const auto elapsed = std::chrono::steady_clock::now() - started;

    REQUIRE(written.find("event: error") != std::string::npos);
    REQUIRE(written.find("timed out") != std::string::npos);
    REQUIRE(elapsed >= std::chrono::seconds(1));
    REQUIRE(elapsed < std::chrono::seconds(5));
}

TEST_CASE("HttpChannel: a vanished client abandons the turn", "[http_channel]") {
    EventBus bus;
    HttpChannel ch(test_config());
    ch.set_event_bus(&bus);

    auto resp = ch.handle_request(chat_request({{"session", "s1"}, {"message", "hi"}}));

    // A writer that reports the peer is gone on the very first frame.
    std::atomic<int> writes{0};
    StreamChunkEvent ev;
    ev.session_id = "s1";
    ev.delta = "x";
    bus.publish(ev);

    resp.stream([&](std::string_view) {
        ++writes;
        return false;
    });

    REQUIRE(writes.load() == 1);  // gave up rather than writing the rest

    // The turn must be gone, so the eventual reply lands nowhere instead of accumulating
    // — and this must not hang or crash.
    ch.send_message("s1", "an answer nobody is listening for");

    // A second request for the same session still works; the abandoned turn left no
    // wreckage behind it.
    auto again = ch.handle_request(chat_request({{"session", "s1"}, {"message", "again"}}));
    REQUIRE(again.status == 200);
}
