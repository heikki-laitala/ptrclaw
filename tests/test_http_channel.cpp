#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "channels/http.hpp"
#include "config.hpp"
#include "event.hpp"
#include "event_bus.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
// struct timeval, for the SO_RCVTIMEO below. glibc's <sys/socket.h> pulls this in
// transitively, so the omission is invisible on the Ubuntu runners CI uses; musl does not,
// and the file fails to compile there with "variable has incomplete type".
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
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

// ── Review fixes: idling, turn identity, typed fields, shutdown ──────

TEST_CASE("HttpChannel: poll_updates waits while idle rather than spinning",
          "[http_channel]") {
    // main.cpp's poll loop has no sleep of its own, so a poll_updates() that returns
    // immediately spins it — an idle deployment would burn a core and re-run session
    // eviction continuously. Both halves matter: wait when idle, return at once when not.
    HttpChannel ch(test_config());

    auto before = std::chrono::steady_clock::now();
    REQUIRE(ch.poll_updates().empty());
    const auto idle_wait = std::chrono::steady_clock::now() - before;
    REQUIRE(idle_wait >= std::chrono::milliseconds(50));

    ch.handle_request(chat_request({{"session", "s1"}, {"message", "hi"}}));
    before = std::chrono::steady_clock::now();
    const auto queued = ch.poll_updates();
    const auto busy_wait = std::chrono::steady_clock::now() - before;
    REQUIRE(queued.size() == 1);
    REQUIRE(busy_wait < std::chrono::milliseconds(50));
}

TEST_CASE("HttpChannel: a second turn for the same session is refused", "[http_channel]") {
    EventBus bus;
    HttpChannel ch(test_config());
    ch.set_event_bus(&bus);

    auto first = ch.handle_request(chat_request({{"session", "s1"}, {"message", "one"}}));
    REQUIRE(first.status == 200);

    // Overlapping turns would share one mailbox: the reply routes by session and so do the
    // deltas, so both streams would race for the first reply.
    REQUIRE(ch.handle_request(chat_request({{"session", "s1"}, {"message", "two"}})).status
            == 409);
    // A different session is unaffected.
    REQUIRE(ch.handle_request(chat_request({{"session", "s2"}, {"message", "x"}})).status
            == 200);
    // And the refused request queued nothing, so no turn runs for it.
    REQUIRE(ch.poll_updates().size() == 2);

    // Once the first turn finishes, the session is usable again.
    std::thread consumer([&] {
        first.stream([](std::string_view) { return true; });
    });
    ch.send_message("s1", "done with one");
    consumer.join();

    REQUIRE(ch.handle_request(chat_request({{"session", "s1"}, {"message", "three"}})).status
            == 200);
}

TEST_CASE("HttpChannel: an abandoned turn stops blocking its session", "[http_channel]") {
    // WebhookServer returns without invoking the producer when the response headers fail
    // to send, and nothing else clears that entry — so with one-turn-per-session enforced,
    // the session would be wedged for the life of the process.
    auto cfg = test_config();
    cfg.turn_timeout_seconds = 1;
    HttpChannel ch(cfg);

    REQUIRE(ch.handle_request(chat_request({{"session", "s1"}, {"message", "one"}})).status
            == 200);
    REQUIRE(ch.handle_request(chat_request({{"session", "s1"}, {"message", "two"}})).status
            == 409);

    std::this_thread::sleep_for(std::chrono::milliseconds(1200));

    REQUIRE(ch.handle_request(chat_request({{"session", "s1"}, {"message", "later"}})).status
            == 200);
}

// ── generated session ids ───────────────────────────────────────
//
// A pod serving many chats may be asked to start one: the caller has nothing to name it
// yet. Off by default, because an id is a routing key the caller has always supplied and a
// channel that silently invents one hides a client bug.

TEST_CASE("HttpChannel: a missing session is still 400 by default", "[http_channel]") {
    HttpChannel ch(test_config());

    auto resp = ch.handle_request(chat_request(json{{"message", "hi"}}));
    REQUIRE(resp.status == 400);
    // Unchanged for every deployment that has not opted in.
    REQUIRE(resp.body.find("session") != std::string::npos);
}

TEST_CASE("HttpChannel: generation accepts a missing, null or empty session",
          "[http_channel]") {
    auto cfg = test_config();
    cfg.generate_session_ids = true;
    HttpChannel ch(cfg);

    REQUIRE(ch.handle_request(chat_request(json{{"message", "hi"}})).status == 200);
    REQUIRE(ch.handle_request(chat_request(
        json{{"session", nullptr}, {"message", "hi"}})).status == 200);
    REQUIRE(ch.handle_request(chat_request(
        json{{"session", ""}, {"message", "hi"}})).status == 200);
}

// The caller cannot continue the conversation without learning the id, and browser
// EventSource clients cannot read response headers — so it has to be in the stream.
TEST_CASE("HttpChannel: a generated id arrives as the first SSE frame", "[http_channel]") {
    auto cfg = test_config();
    cfg.generate_session_ids = true;
    cfg.turn_timeout_seconds = 1;   // nothing will answer this turn
    HttpChannel ch(cfg);

    auto resp = ch.handle_request(chat_request(json{{"message", "hi"}}));
    REQUIRE(resp.status == 200);
    REQUIRE(resp.stream != nullptr);

    std::string written;
    resp.stream([&written](std::string_view chunk) {
        written.append(chunk);
        return true;
    });

    REQUIRE(written.rfind("event: session\n", 0) == 0);
    auto first_end = written.find("\n\n");
    REQUIRE(first_end != std::string::npos);
    auto payload = written.substr(0, first_end);
    auto data_at = payload.find("data: ");
    REQUIRE(data_at != std::string::npos);
    auto session = json::parse(payload.substr(data_at + 6));
    REQUIRE(session.contains("session"));
    REQUIRE_FALSE(session["session"].get<std::string>().empty());

    // The queued message must carry the same id, or the reply routes nowhere.
    auto queued = ch.poll_updates();
    REQUIRE(queued.size() == 1);
    REQUIRE(queued[0].sender == session["session"].get<std::string>());
}

TEST_CASE("HttpChannel: an explicit session gets no session frame", "[http_channel]") {
    auto cfg = test_config();
    cfg.generate_session_ids = true;
    cfg.turn_timeout_seconds = 1;
    HttpChannel ch(cfg);

    auto resp = ch.handle_request(chat_request(
        json{{"session", "task-42"}, {"message", "hi"}}));
    REQUIRE(resp.stream != nullptr);

    std::string written;
    resp.stream([&written](std::string_view chunk) {
        written.append(chunk);
        return true;
    });

    // Nothing was invented, so nothing needs announcing — the stream a current client sees
    // is byte-for-byte what it saw before.
    REQUIRE(written.find("event: session") == std::string::npos);
}

// A client that disconnects before the session frame lands makes write() return false. If
// that result is ignored the turn still waits out turn_timeout_seconds, holding a connection
// thread the whole time — repeated early disconnects would then exhaust max_connections.
TEST_CASE("HttpChannel: a failed session frame abandons the turn at once",
          "[http_channel]") {
    auto cfg = test_config();
    cfg.generate_session_ids = true;
    cfg.turn_timeout_seconds = 30;   // long enough that waiting it out is unmistakable
    HttpChannel ch(cfg);

    auto resp = ch.handle_request(chat_request(json{{"message", "hi"}}));
    REQUIRE(resp.status == 200);
    REQUIRE(resp.stream != nullptr);

    int writes = 0;
    auto started = std::chrono::steady_clock::now();
    resp.stream([&writes](std::string_view) {
        ++writes;
        return false;   // the client is gone
    });
    auto elapsed = std::chrono::steady_clock::now() - started;

    // One attempt, then out — not thirty seconds of waiting for a reply nobody can receive.
    REQUIRE(writes == 1);
    REQUIRE(elapsed < std::chrono::seconds(5));

    // And the turn is released rather than left in flight, so the session is not wedged
    // until the stale-turn timeout.
    auto again = ch.handle_request(chat_request(json{{"session", "explicit"},
                                                    {"message", "hi"}}));
    REQUIRE(again.status == 200);
}

TEST_CASE("HttpChannel: generated ids differ between requests", "[http_channel]") {
    auto cfg = test_config();
    cfg.generate_session_ids = true;
    HttpChannel ch(cfg);

    REQUIRE(ch.handle_request(chat_request(json{{"message", "one"}})).status == 200);
    REQUIRE(ch.handle_request(chat_request(json{{"message", "two"}})).status == 200);

    auto queued = ch.poll_updates();
    REQUIRE(queued.size() == 2);
    // Two callers with no id must not land in one conversation — and a single id would
    // also trip the one-turn-per-session refusal.
    REQUIRE(queued[0].sender != queued[1].sender);
}

TEST_CASE("HttpChannel: wrongly typed required fields are 400, not 500", "[http_channel]") {
    // nlohmann's value<std::string>() throws on a type mismatch rather than returning the
    // default, so these used to escape to the handler wrapper and surface as a 500 for
    // what is plainly a malformed request.
    HttpChannel ch(test_config());

    REQUIRE(ch.handle_request(chat_request({{"session", 1}, {"message", "hi"}})).status == 400);
    REQUIRE(ch.handle_request(chat_request({{"session", "s1"}, {"message", 42}})).status == 400);
    REQUIRE(ch.handle_request(chat_request({{"session", json::array()}, {"message", "hi"}}))
                .status == 400);
    REQUIRE(ch.poll_updates().empty());
}

TEST_CASE("HttpChannel: the tool role is refused rather than half-supported",
          "[http_channel]") {
    HttpChannel ch(test_config());
    auto resp = ch.handle_request(chat_request({
        {"session", "s1"},
        {"message", "hi"},
        {"history", json::array({json{{"role", "tool"}, {"content", "result"}}})},
    }));
    REQUIRE(resp.status == 400);
    REQUIRE(resp.body.find("tool_call_id") != std::string::npos);
    REQUIRE(ch.poll_updates().empty());
}

// ── Shutdown, over a real connection ────────────────────────────────
//
// This one cannot be faked. WebhookServer::stop() joins its connection threads, and that
// join is exactly what turns a parked stream into a stalled shutdown — so the test needs a
// real server and a real client.

namespace {

// initialize() throws when the port is taken, so scan for a free one rather than risking a
// collision with a parallel run or a TIME_WAIT leftover.
std::unique_ptr<HttpChannel> start_on_free_port(HttpChannelConfig cfg, uint16_t& out_port) {
    for (uint16_t p = 18800; p < 18830; ++p) {
        cfg.listen = "127.0.0.1:" + std::to_string(p);
        auto ch = std::make_unique<HttpChannel>(cfg);
        try {
            ch->initialize();
            out_port = p;
            return ch;
        } catch (const std::exception&) {
            continue;
        }
    }
    throw std::runtime_error("no free port for the http channel test");
}

int connect_to(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    struct timeval tv{6, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = ::inet_addr("127.0.0.1");
    REQUIRE(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    return fd;
}

std::string recv_until(int fd, const std::string& needle) {
    std::string acc;
    char buf[2048];
    while (acc.find(needle) == std::string::npos) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        acc.append(buf, static_cast<size_t>(n));
    }
    return acc;
}

} // namespace

TEST_CASE("HttpChannel: shutdown does not wait for the turn timeout", "[http_channel]") {
    auto cfg = test_config();
    // Far longer than any acceptable shutdown, and longer than Kubernetes'
    // terminationGracePeriodSeconds — so a stall here is a SIGKILL in production.
    cfg.turn_timeout_seconds = 60;
    cfg.max_connections = 4;

    uint16_t port = 0;
    auto ch = start_on_free_port(cfg, port);

    int fd = connect_to(port);
    const std::string body = R"({"session":"s1","message":"hi"})";
    const std::string request =
        "POST /chat HTTP/1.1\r\nHost: t\r\nContent-Length: " +
        std::to_string(body.size()) + "\r\n\r\n" + body;
    REQUIRE(::send(fd, request.data(), request.size(), 0) ==
            static_cast<ssize_t>(request.size()));

    // Headers are written before the producer runs, so this proves the request reached
    // stream_turn() and is now parked waiting for a reply that will never come.
    REQUIRE_FALSE(recv_until(fd, "\r\n\r\n").empty());

    const auto before = std::chrono::steady_clock::now();
    ch.reset();  // destructor: release pending turns, then stop() joins the threads
    const auto elapsed = std::chrono::steady_clock::now() - before;

    REQUIRE(elapsed < std::chrono::seconds(5));
    ::close(fd);
}

// ── Ending a session ────────────────────────────────────────────────
//
// A context manager knows when a task is over; the pod cannot infer it. Without this the
// only exit was the idle timer, which keeps the session's files for as long as it keeps the
// session — and then keeps the files forever.

namespace {

WebhookRequest end_request(const json& body) {
    WebhookRequest req;
    req.method = "POST";
    req.path = "/session/end";
    req.body = body.dump();
    return req;
}

} // namespace

TEST_CASE("HttpChannel: ending a session asks for cleanup", "[http_channel]") {
    EventBus bus;
    HttpChannel ch(test_config());
    ch.set_event_bus(&bus);

    std::vector<std::string> ended;
    subscribe<SessionEndRequestedEvent>(bus, [&ended](const SessionEndRequestedEvent& ev) {
        ended.push_back(ev.session_id);
    });

    auto resp = ch.handle_request(end_request({{"session", "s1"}}));
    // 202, not 200: the session is freed once no turn is in flight anywhere in the pod, so
    // the request is accepted rather than completed.
    REQUIRE(resp.status == 202);
    REQUIRE(json::parse(resp.body)["session"] == "s1");
    REQUIRE(ended == std::vector<std::string>{"s1"});
    // Not a turn: nothing may reach the agent, and no reply is owed.
    REQUIRE(ch.poll_updates().empty());
}

TEST_CASE("HttpChannel: ending validates the session id", "[http_channel]") {
    HttpChannel ch(test_config());

    REQUIRE(ch.handle_request(end_request(json::object())).status == 400);
    REQUIRE(ch.handle_request(end_request({{"session", ""}})).status == 400);
    REQUIRE(ch.handle_request(end_request({{"session", 7}})).status == 400);
    // generate_session_ids exists so a *new* conversation need not name itself; there is no
    // such thing as ending a session nobody named.
    auto cfg = test_config();
    cfg.generate_session_ids = true;
    HttpChannel gen(cfg);
    REQUIRE(gen.handle_request(end_request(json::object())).status == 400);
}

TEST_CASE("HttpChannel: ending requires the secret and the right method",
          "[http_channel]") {
    auto cfg = test_config();
    cfg.secret = "s3cret";
    EventBus bus;
    HttpChannel ch(cfg);
    ch.set_event_bus(&bus);

    std::vector<std::string> ended;
    subscribe<SessionEndRequestedEvent>(bus, [&ended](const SessionEndRequestedEvent& ev) {
        ended.push_back(ev.session_id);
    });

    // Deleting another tenant's work is exactly what the secret is for.
    auto req = end_request({{"session", "s1"}});
    REQUIRE(ch.handle_request(req).status == 403);
    REQUIRE(ended.empty());

    WebhookRequest wrong_method;
    wrong_method.method = "GET";
    wrong_method.path = "/session/end";
    wrong_method.headers["authorization"] = "Bearer s3cret";
    REQUIRE(ch.handle_request(wrong_method).status == 405);
    REQUIRE(ended.empty());

    req.headers["authorization"] = "Bearer s3cret";
    REQUIRE(ch.handle_request(req).status == 202);
    REQUIRE(ended == std::vector<std::string>{"s1"});
}

TEST_CASE("HttpChannel: ending a session clears its in-flight guard", "[http_channel]") {
    // A caller that ends a session whose turn never completed — the client hung up, or the
    // turn is being abandoned — must not leave the id wedged behind a 409 for the lifetime
    // of the process.
    EventBus bus;
    HttpChannel ch(test_config());
    ch.set_event_bus(&bus);

    ch.handle_request(chat_request({{"session", "s1"}, {"message", "hi"}}));
    REQUIRE(ch.handle_request(chat_request({{"session", "s1"}, {"message", "again"}}))
                .status == 409);

    ch.handle_request(end_request({{"session", "s1"}}));
    REQUIRE(ch.handle_request(chat_request({{"session", "s1"}, {"message", "fresh"}}))
                .status == 200);
}

TEST_CASE("HttpChannel: ending a session releases a waiting stream", "[http_channel]") {
    // The connection thread parks in stream_turn() until something notifies it. Ending the
    // session erases the turn it is waiting on, and without a notify it would sleep out the
    // whole turn timeout holding a connection slot — so a caller cancelling its own tasks
    // could exhaust max_connections while every one of them was already finished.
    auto cfg = test_config();
    cfg.turn_timeout_seconds = 30;  // far longer than this should take
    EventBus bus;
    HttpChannel ch(cfg);
    ch.set_event_bus(&bus);

    auto resp = ch.handle_request(chat_request({{"session", "s1"}, {"message", "hi"}}));
    REQUIRE(resp.status == 200);

    std::atomic<bool> returned{false};
    std::thread consumer([&] {
        resp.stream([](std::string_view) { return true; });
        returned = true;
    });

    // Give the consumer time to reach the wait before the turn is taken away.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ch.handle_request(end_request({{"session", "s1"}}));

    for (int i = 0; i < 200 && !returned; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(returned);
    consumer.join();
}

TEST_CASE("HttpChannel: an ended stream cannot consume the next turn's tokens",
          "[http_channel]") {
    // WebhookServer invokes the producer after handle_request() has returned, so a stream
    // can begin writing when its turn has already been ended and the id reused. Identifying
    // a turn by session id alone, the old connection would then stream the new turn's tokens
    // to the wrong client — and its own cleanup would erase a turn that is still live.
    //
    // Ordered rather than raced: the second turn is registered before the first stream runs
    // at all, so the hazard is reached every time instead of when the scheduler allows.
    auto cfg = test_config();
    cfg.turn_timeout_seconds = 2;  // bounds the failing case; the fixed one returns at once
    EventBus bus;
    HttpChannel ch(cfg);
    ch.set_event_bus(&bus);

    auto first = ch.handle_request(chat_request({{"session", "s1"}, {"message", "one"}}));
    REQUIRE(first.status == 200);

    ch.handle_request(end_request({{"session", "s1"}}));

    // The id is reused, as a context manager retrying a cancelled task would.
    auto second = ch.handle_request(chat_request({{"session", "s1"}, {"message", "two"}}));
    REQUIRE(second.status == 200);
    StreamChunkEvent chunk;
    chunk.session_id = "s1";
    chunk.delta = "SECOND-TURN-TOKEN";
    bus.publish(chunk);

    std::string seen;
    first.stream([&](std::string_view piece) {
        seen.append(piece);
        return true;
    });
    // Its own turn is gone, so it has nothing to say and says nothing.
    REQUIRE(seen.find("SECOND-TURN-TOKEN") == std::string::npos);

    // And the second turn still has its token to deliver: the dead stream must not have
    // erased somebody else's live turn on the way out.
    std::string second_seen;
    std::thread reader([&] {
        second.stream([&](std::string_view piece) {
            second_seen.append(piece);
            return true;
        });
    });
    ch.send_message("s1", "second done");
    reader.join();
    REQUIRE(second_seen.find("SECOND-TURN-TOKEN") != std::string::npos);
}

TEST_CASE("HttpChannel: a serving build admits more callers than workers",
          "[http_channel]") {
    // max_connections bounds concurrent *connections*; workers bound concurrent *turns*.
    // Past the connection limit the acceptor stops accepting and the backlog queues, so a
    // limit at or below the worker count turns waiting callers into stalled ones.
    HttpChannelConfig cfg;
#ifdef PTRCLAW_HAS_SERVING
    REQUIRE(cfg.max_connections > Config{}.workers);
#else
    REQUIRE(cfg.max_connections == 8);
#endif
}

// ── Accept queue depth ──────────────────────────────────────────

TEST_CASE("listen_backlog: tracks max_connections", "[http_channel]") {
    // The acceptor leaves an over-capacity connection in the kernel queue rather than
    // accepting it to answer 503, so that queue is the waiting room. A constant 16 capped a
    // pod at 16 waiters no matter how high max_connections went — measured as exactly 48
    // requests served out of 64 fired at max_connections=32.
    REQUIRE(listen_backlog(32) == 32);
    REQUIRE(listen_backlog(256) == 256);
}

TEST_CASE("listen_backlog: keeps a floor for small servers", "[http_channel]") {
    // A single-connection server still wants somewhere for the next caller to wait instead
    // of being reset while the current one is served.
    REQUIRE(listen_backlog(1) == 16);
    REQUIRE(listen_backlog(0) == 16);
    REQUIRE(listen_backlog(16) == 16);
}

// ── Tool calls: out through the stream, back in as history ──────
//
// A pod whose transcript is owned outside it has to be able to hand over a full turn and
// take it back. Text alone is not a full turn: a conversation that ran tools replays as
// though it never did, and the model loses what the tool told it.

TEST_CASE("HttpChannel: tool calls and results reach the stream", "[http_channel]") {
    auto cfg = test_config();
    cfg.turn_timeout_seconds = 5;
    EventBus bus;
    HttpChannel ch(cfg);
    ch.set_event_bus(&bus);

    auto resp = ch.handle_request(chat_request({{"session", "s1"}, {"message", "read it"}}));
    REQUIRE(resp.status == 200);

    ToolCallRequestEvent call;
    call.session_id = "s1";
    call.tool_call_id = "call_1";
    call.tool_name = "file_read";
    call.arguments_json = R"({"path":"notes.md"})";
    bus.publish(call);

    ToolCallResultEvent result;
    result.session_id = "s1";
    result.tool_call_id = "call_1";
    result.tool_name = "file_read";
    result.success = true;
    result.output = "the file body";
    bus.publish(result);

    std::string seen;
    std::thread consumer([&] {
        resp.stream([&](std::string_view chunk) {
            seen.append(chunk);
            return true;
        });
    });
    ch.send_message("s1", "done reading");
    consumer.join();

    REQUIRE(seen.find("event: tool_call") != std::string::npos);
    REQUIRE(seen.find("event: tool_result") != std::string::npos);
    // The id is what makes the pair reconstructable; without it the caller cannot push
    // either message back.
    REQUIRE(seen.find("call_1") != std::string::npos);
    REQUIRE(seen.find("file_read") != std::string::npos);
    REQUIRE(seen.find("the file body") != std::string::npos);
    // Ordering matters as much as presence: the call has to precede its result.
    REQUIRE(seen.find("event: tool_call") < seen.find("event: tool_result"));
}

TEST_CASE("HttpChannel: a tool exchange is accepted back as history", "[http_channel]") {
    HttpChannel ch(test_config());

    auto req = chat_request({
        {"session", "s1"},
        {"message", "and the second file?"},
        {"history", {
            {{"role", "user"}, {"content", "read notes.md"}},
            {{"role", "assistant"}, {"content", ""}, {"tool_calls", {
                {{"id", "call_1"}, {"name", "file_read"},
                 {"arguments", R"({"path":"notes.md"})"}}}}},
            {{"role", "tool"}, {"content", "the file body"},
             {"tool_call_id", "call_1"}, {"name", "file_read"}},
            {{"role", "assistant"}, {"content", "It says the deadline is April."}},
        }},
    });
    REQUIRE(ch.handle_request(req).status == 200);
}

TEST_CASE("HttpChannel: an unpaired tool result is refused", "[http_channel]") {
    // A result whose call is not in the window is what providers reject outright — OpenAI
    // refuses a 'tool' message that answers nothing. Better a 400 naming the id than a
    // provider error the caller cannot act on.
    HttpChannel ch(test_config());

    auto req = chat_request({
        {"session", "s1"},
        {"message", "hi"},
        {"history", {
            {{"role", "user"}, {"content", "read notes.md"}},
            {{"role", "tool"}, {"content", "the file body"},
             {"tool_call_id", "call_missing"}, {"name", "file_read"}},
        }},
    });
    auto resp = ch.handle_request(req);
    REQUIRE(resp.status == 400);
    REQUIRE(resp.body.find("call_missing") != std::string::npos);
}

TEST_CASE("HttpChannel: a tool call with no result is refused", "[http_channel]") {
    // The other half of the pairing. An assistant turn that called a tool and no result to
    // go with it leaves the provider waiting for an answer that never comes.
    HttpChannel ch(test_config());

    auto req = chat_request({
        {"session", "s1"},
        {"message", "hi"},
        {"history", {
            {{"role", "assistant"}, {"content", ""}, {"tool_calls", {
                {{"id", "call_1"}, {"name", "file_read"}, {"arguments", "{}"}}}}},
            {{"role", "assistant"}, {"content", "done"}},
        }},
    });
    auto resp = ch.handle_request(req);
    REQUIRE(resp.status == 400);
    REQUIRE(resp.body.find("call_1") != std::string::npos);
}

TEST_CASE("HttpChannel: a tool entry without an id is refused", "[http_channel]") {
    HttpChannel ch(test_config());
    auto req = chat_request({
        {"session", "s1"},
        {"message", "hi"},
        {"history", {
            {{"role", "tool"}, {"content", "output"}},
        }},
    });
    auto resp = ch.handle_request(req);
    REQUIRE(resp.status == 400);
    REQUIRE(resp.body.find("tool_call_id") != std::string::npos);
}

TEST_CASE("HttpChannel: a tool result separated from its call is refused",
          "[http_channel]") {
    // Pairing is not enough — position matters. OpenAI requires the results to follow the
    // assistant message that made the calls; anything between them is rejected upstream.
    // A window trimmer that drops turns oldest-first can produce exactly this, so it is
    // caught here where the error can say which call is stranded.
    HttpChannel ch(test_config());

    auto req = chat_request({
        {"session", "s1"},
        {"message", "hi"},
        {"history", {
            {{"role", "assistant"}, {"content", ""}, {"tool_calls", {
                {{"id", "call_1"}, {"name", "file_read"}, {"arguments", "{}"}}}}},
            {{"role", "user"}, {"content", "are you there?"}},
            {{"role", "tool"}, {"content", "the file body"},
             {"tool_call_id", "call_1"}, {"name", "file_read"}},
        }},
    });
    auto resp = ch.handle_request(req);
    REQUIRE(resp.status == 400);
    REQUIRE(resp.body.find("call_1") != std::string::npos);
}

TEST_CASE("HttpChannel: several calls in one turn are answered together", "[http_channel]") {
    // Multi-tool rounds are the normal case, and all the results belong to the same
    // assistant message rather than being spread across the window.
    HttpChannel ch(test_config());

    auto req = chat_request({
        {"session", "s1"},
        {"message", "hi"},
        {"history", {
            {{"role", "user"}, {"content", "read both files"}},
            {{"role", "assistant"}, {"content", ""}, {"tool_calls", {
                {{"id", "call_1"}, {"name", "file_read"}, {"arguments", "{}"}},
                {{"id", "call_2"}, {"name", "file_read"}, {"arguments", "{}"}}}}},
            {{"role", "tool"}, {"content", "first"},  {"tool_call_id", "call_1"}},
            {{"role", "tool"}, {"content", "second"}, {"tool_call_id", "call_2"}},
            {{"role", "assistant"}, {"content", "Both read."}},
        }},
    });
    REQUIRE(ch.handle_request(req).status == 200);
}

TEST_CASE("HttpChannel: tool frames carry the batch they belong to", "[http_channel]") {
    // A batch's calls are published one at a time and results arrive as they finish, so a
    // fast result can land before the next call. Without the batch a transcript owner
    // cannot tell one interleaved round from two, and the assistant `tool_calls` array it
    // has to rebuild groups exactly one round.
    EventBus bus;
    HttpChannel ch(test_config());
    ch.set_event_bus(&bus);
    auto resp = ch.handle_request(chat_request({{"session", "s1"}, {"message", "go"}}));

    ToolCallRequestEvent call;
    call.session_id = "s1";
    call.batch_id = "batch-7";
    call.tool_call_id = "call_1";
    call.tool_name = "file_read";
    call.arguments_json = "{}";
    bus.publish(call);

    ToolCallResultEvent done;
    done.session_id = "s1";
    done.batch_id = "batch-7";
    done.tool_call_id = "call_1";
    done.tool_name = "file_read";
    done.success = true;
    done.output = "body";
    bus.publish(done);

    std::string seen;
    std::thread consumer([&] {
        resp.stream([&](std::string_view c) { seen.append(c); return true; });
    });
    ch.send_message("s1", "ok");
    consumer.join();

    REQUIRE(seen.find("batch-7") != std::string::npos);
    // Both halves carry it, or the pairing cannot be reconstructed from the stream alone.
    REQUIRE(seen.find("batch-7") != seen.rfind("batch-7"));
}

TEST_CASE("HttpChannel: a failed tool exports what history holds", "[http_channel]") {
    // The agent stores a failure as "Error: ..." — the role alone does not tell the model
    // it failed. Exporting the raw output would let a caller replay content the agent never
    // saw, with the failure silently erased.
    EventBus bus;
    HttpChannel ch(test_config());
    ch.set_event_bus(&bus);
    auto resp = ch.handle_request(chat_request({{"session", "s1"}, {"message", "go"}}));

    ToolCallResultEvent failed;
    failed.session_id = "s1";
    failed.tool_call_id = "call_1";
    failed.tool_name = "file_read";
    failed.success = false;
    failed.output = "no such file";
    bus.publish(failed);

    std::string seen;
    std::thread consumer([&] {
        resp.stream([&](std::string_view c) { seen.append(c); return true; });
    });
    ch.send_message("s1", "ok");
    consumer.join();

    REQUIRE(seen.find("Error: no such file") != std::string::npos);
    REQUIRE(seen.find("\"success\":false") != std::string::npos);
}

TEST_CASE("HttpChannel: a late result cannot contradict the one already reported",
          "[http_channel]") {
    // A tool cancelled for exceeding the timeout can still finish and publish. History
    // already holds the synthesised timeout, so emitting the late one would hand the caller
    // a transcript the agent never had — and two results for one call is a window no
    // provider accepts.
    EventBus bus;
    HttpChannel ch(test_config());
    ch.set_event_bus(&bus);
    auto resp = ch.handle_request(chat_request({{"session", "s1"}, {"message", "go"}}));

    ToolCallResultEvent timed_out;
    timed_out.session_id = "s1";
    timed_out.tool_call_id = "call_1";
    timed_out.tool_name = "shell";
    timed_out.success = false;
    timed_out.output = "Tool call timed out after 120s";
    bus.publish(timed_out);

    ToolCallResultEvent late = timed_out;
    late.success = true;
    late.output = "finished eventually";
    bus.publish(late);

    std::string seen;
    std::thread consumer([&] {
        resp.stream([&](std::string_view c) { seen.append(c); return true; });
    });
    ch.send_message("s1", "ok");
    consumer.join();

    REQUIRE(seen.find("timed out") != std::string::npos);
    REQUIRE(seen.find("finished eventually") == std::string::npos);
}

TEST_CASE("HttpChannel: arguments that are not JSON are refused", "[http_channel]") {
    // Unchecked, this fails far away and quietly: Anthropic drops a tool_use block whose
    // arguments will not parse but keeps the tool_result answering it.
    HttpChannel ch(test_config());
    auto resp = ch.handle_request(chat_request({
        {"session", "s1"},
        {"message", "hi"},
        {"history", {
            {{"role", "assistant"}, {"content", ""}, {"tool_calls", {
                {{"id", "call_1"}, {"name", "file_read"}, {"arguments", "not-json"}}}}},
            {{"role", "tool"}, {"content", "x"}, {"tool_call_id", "call_1"}},
        }},
    }));
    REQUIRE(resp.status == 400);
    REQUIRE(resp.body.find("call_1") != std::string::npos);
    REQUIRE(resp.body.find("JSON") != std::string::npos);
}

// ── Machine-readable history errors ─────────────────────────────
//
// The distinction that matters to a caller is not the sentence: it is whether retrying with
// a repaired window can work. A window split by a size trimmer is worth re-trimming and
// sending again; a malformed request is not, and retrying it is a loop. Both were 400 with
// English text, so telling them apart meant matching on prose that could be reworded.

TEST_CASE("HttpChannel: a split tool pair is reported as unbalanced", "[http_channel]") {
    HttpChannel ch(test_config());
    auto resp = ch.handle_request(chat_request({
        {"session", "s1"}, {"message", "hi"},
        {"history", {
            {{"role", "assistant"}, {"content", ""}, {"tool_calls", {
                {{"id", "call_1"}, {"name", "file_read"}, {"arguments", "{}"}}}}},
            {{"role", "assistant"}, {"content", "done"}},
        }},
    }));
    REQUIRE(resp.status == 400);
    auto body = json::parse(resp.body);
    REQUIRE(body["code"] == "history_unbalanced");
    // Which call to repair, without parsing the sentence.
    REQUIRE(body["tool_call_id"] == "call_1");
    REQUIRE_FALSE(body["error"].get<std::string>().empty());
}

TEST_CASE("HttpChannel: an orphaned result is reported as unbalanced", "[http_channel]") {
    HttpChannel ch(test_config());
    auto resp = ch.handle_request(chat_request({
        {"session", "s1"}, {"message", "hi"},
        {"history", {
            {{"role", "tool"}, {"content", "x"}, {"tool_call_id", "call_9"}},
        }},
    }));
    REQUIRE(resp.status == 400);
    auto body = json::parse(resp.body);
    REQUIRE(body["code"] == "history_unbalanced");
    REQUIRE(body["tool_call_id"] == "call_9");
}

TEST_CASE("HttpChannel: a schema fault is not reported as unbalanced", "[http_channel]") {
    // Re-trimming cannot fix these, so they must not look like the retryable case.
    HttpChannel ch(test_config());

    auto bad_args = ch.handle_request(chat_request({
        {"session", "s1"}, {"message", "hi"},
        {"history", {
            {{"role", "assistant"}, {"content", ""}, {"tool_calls", {
                {{"id", "call_1"}, {"name", "file_read"}, {"arguments", "not-json"}}}}},
            {{"role", "tool"}, {"content", "x"}, {"tool_call_id", "call_1"}},
        }},
    }));
    REQUIRE(bad_args.status == 400);
    REQUIRE(json::parse(bad_args.body)["code"] == "history_malformed");

    auto unknown_role = ch.handle_request(chat_request({
        {"session", "s1"}, {"message", "hi"},
        {"history", {{{"role", "narrator"}, {"content", "x"}}}},
    }));
    REQUIRE(json::parse(unknown_role.body)["code"] == "history_malformed");

    auto no_id = ch.handle_request(chat_request({
        {"session", "s1"}, {"message", "hi"},
        {"history", {{{"role", "tool"}, {"content", "x"}}}},
    }));
    REQUIRE(json::parse(no_id.body)["code"] == "history_malformed");
}
