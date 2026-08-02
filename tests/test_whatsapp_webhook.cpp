#include <catch2/catch_test_macros.hpp>
#include "channels/whatsapp.hpp"
#include "channels/webhook_server.hpp"
#include "mock_http_client.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
// struct timeval, for the SO_RCVTIMEO below — the same omission #110 fixed in
// webhook_server.cpp and the http channel's test. glibc's <sys/socket.h> supplies it
// transitively; musl does not, and this file fails to compile there.
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>

using namespace ptrclaw;

static WhatsAppConfig make_webhook_config() {
    WhatsAppConfig cfg;
    cfg.access_token    = "test-token";
    cfg.phone_number_id = "123456";
    cfg.verify_token    = "verify-secret";
    cfg.allow_from      = {"*"};
    cfg.webhook_listen  = "127.0.0.1:8080";
    cfg.webhook_max_body = 65536;
    return cfg;
}

// ── parse_listen_addr ─────────────────────────────────────────────────────────

TEST_CASE("parse_listen_addr: valid host:port", "[whatsapp_webhook]") {
    std::string host; uint16_t port;
    REQUIRE(parse_listen_addr("127.0.0.1:8080", host, port));
    REQUIRE(host == "127.0.0.1");
    REQUIRE(port == 8080);
}

TEST_CASE("parse_listen_addr: missing colon returns false", "[whatsapp_webhook]") {
    std::string host; uint16_t port;
    REQUIRE_FALSE(parse_listen_addr("127.0.0.1", host, port));
}

TEST_CASE("parse_listen_addr: non-numeric port returns false", "[whatsapp_webhook]") {
    std::string host; uint16_t port;
    REQUIRE_FALSE(parse_listen_addr("127.0.0.1:notaport", host, port));
}

TEST_CASE("parse_listen_addr: empty string returns false", "[whatsapp_webhook]") {
    std::string host; uint16_t port;
    REQUIRE_FALSE(parse_listen_addr("", host, port));
}

TEST_CASE("parse_listen_addr: port 0 is rejected", "[whatsapp_webhook]") {
    std::string host; uint16_t port;
    REQUIRE_FALSE(parse_listen_addr("127.0.0.1:0", host, port));
}

// ── WebhookRequest::query_param ───────────────────────────────────────────────

TEST_CASE("WebhookRequest::query_param: basic lookup", "[whatsapp_webhook]") {
    WebhookRequest req;
    req.query_params = {{"hub.mode", "subscribe"}, {"hub.verify_token", "secret"}, {"hub.challenge", "abc123"}};
    REQUIRE(req.query_param("hub.mode") == "subscribe");
    REQUIRE(req.query_param("hub.verify_token") == "secret");
    REQUIRE(req.query_param("hub.challenge") == "abc123");
}

TEST_CASE("WebhookRequest::query_param: missing key returns empty", "[whatsapp_webhook]") {
    WebhookRequest req;
    req.query_params = {{"key", "val"}};
    REQUIRE(req.query_param("other").empty());
}

TEST_CASE("WebhookRequest::query_param: empty query params", "[whatsapp_webhook]") {
    WebhookRequest req;
    REQUIRE(req.query_param("anything").empty());
}

// ── GET verify handshake ──────────────────────────────────────────────────────

TEST_CASE("WhatsApp webhook: GET verify returns challenge on match", "[whatsapp_webhook]") {
    MockHttpClient http;
    WhatsAppChannel ch(make_webhook_config(), http);

    WebhookRequest req;
    req.method = "GET";
    req.path   = "/webhook";
    req.query_params = {{"hub.mode", "subscribe"}, {"hub.verify_token", "verify-secret"}, {"hub.challenge", "abc123"}};

    auto resp = ch.handle_webhook_request(req);
    REQUIRE(resp.status == 200);
    REQUIRE(resp.body == "abc123");
}

TEST_CASE("WhatsApp webhook: GET verify wrong token returns 403", "[whatsapp_webhook]") {
    MockHttpClient http;
    WhatsAppChannel ch(make_webhook_config(), http);

    WebhookRequest req;
    req.method = "GET";
    req.path   = "/webhook";
    req.query_params = {{"hub.mode", "subscribe"}, {"hub.verify_token", "wrong"}, {"hub.challenge", "abc123"}};

    auto resp = ch.handle_webhook_request(req);
    REQUIRE(resp.status == 403);
}

TEST_CASE("WhatsApp webhook: GET verify missing mode returns 403", "[whatsapp_webhook]") {
    MockHttpClient http;
    WhatsAppChannel ch(make_webhook_config(), http);

    WebhookRequest req;
    req.method = "GET";
    req.path   = "/webhook";
    req.query_params = {{"hub.verify_token", "verify-secret"}, {"hub.challenge", "abc123"}}; // no hub.mode

    auto resp = ch.handle_webhook_request(req);
    REQUIRE(resp.status == 403);
}

TEST_CASE("WhatsApp webhook: GET verify wrong mode returns 403", "[whatsapp_webhook]") {
    MockHttpClient http;
    WhatsAppChannel ch(make_webhook_config(), http);

    WebhookRequest req;
    req.method = "GET";
    req.path   = "/webhook";
    req.query_params = {{"hub.mode", "unsubscribe"}, {"hub.verify_token", "verify-secret"}, {"hub.challenge", "x"}};

    auto resp = ch.handle_webhook_request(req);
    REQUIRE(resp.status == 403);
}

TEST_CASE("WhatsApp webhook: GET verify with empty verify_token returns 403", "[whatsapp_webhook]") {
    MockHttpClient http;
    auto cfg = make_webhook_config();
    cfg.verify_token = "";
    WhatsAppChannel ch(cfg, http);

    WebhookRequest req;
    req.method = "GET";
    req.path   = "/webhook";
    req.query_params = {{"hub.mode", "subscribe"}, {"hub.verify_token", ""}, {"hub.challenge", "x"}};

    auto resp = ch.handle_webhook_request(req);
    REQUIRE(resp.status == 403);
}

// ── POST without shared secret configured ────────────────────────────────────

TEST_CASE("WhatsApp webhook: POST no secret configured returns 200", "[whatsapp_webhook]") {
    MockHttpClient http;
    auto cfg = make_webhook_config();
    cfg.webhook_secret = "";
    WhatsAppChannel ch(cfg, http);

    WebhookRequest req;
    req.method = "POST";
    req.path   = "/webhook";
    req.body   = R"({"entry":[]})";

    auto resp = ch.handle_webhook_request(req);
    REQUIRE(resp.status == 200);
}

// ── POST with shared secret ───────────────────────────────────────────────────

TEST_CASE("WhatsApp webhook: POST correct secret returns 200", "[whatsapp_webhook]") {
    MockHttpClient http;
    auto cfg = make_webhook_config();
    cfg.webhook_secret = "proxy-secret";
    WhatsAppChannel ch(cfg, http);

    WebhookRequest req;
    req.method = "POST";
    req.path   = "/webhook";
    req.headers["x-webhook-secret"] = "proxy-secret";
    req.body   = R"({"entry":[]})";

    auto resp = ch.handle_webhook_request(req);
    REQUIRE(resp.status == 200);
}

TEST_CASE("WhatsApp webhook: POST wrong secret returns 403", "[whatsapp_webhook]") {
    MockHttpClient http;
    auto cfg = make_webhook_config();
    cfg.webhook_secret = "proxy-secret";
    WhatsAppChannel ch(cfg, http);

    WebhookRequest req;
    req.method = "POST";
    req.path   = "/webhook";
    req.headers["x-webhook-secret"] = "wrong";
    req.body   = R"({"entry":[]})";

    auto resp = ch.handle_webhook_request(req);
    REQUIRE(resp.status == 403);
}

TEST_CASE("WhatsApp webhook: POST missing secret header returns 403", "[whatsapp_webhook]") {
    MockHttpClient http;
    auto cfg = make_webhook_config();
    cfg.webhook_secret = "proxy-secret";
    WhatsAppChannel ch(cfg, http);

    WebhookRequest req;
    req.method = "POST";
    req.path   = "/webhook";
    // No x-webhook-secret header
    req.body   = R"({"entry":[]})";

    auto resp = ch.handle_webhook_request(req);
    REQUIRE(resp.status == 403);
}

// ── POST payload ingestion ────────────────────────────────────────────────────

static const char* kValidPayload = R"({
    "entry": [{
        "changes": [{
            "value": {
                "messages": [{
                    "from": "1234567890",
                    "type": "text",
                    "text": {"body": "Hello webhook!"},
                    "timestamp": "1700000000"
                }]
            }
        }]
    }]
})";

TEST_CASE("WhatsApp webhook: POST valid payload queues message", "[whatsapp_webhook]") {
    MockHttpClient http;
    auto cfg = make_webhook_config();
    cfg.webhook_secret = "";
    WhatsAppChannel ch(cfg, http);

    WebhookRequest req;
    req.method = "POST";
    req.path   = "/webhook";
    req.body   = kValidPayload;

    auto resp = ch.handle_webhook_request(req);
    REQUIRE(resp.status == 200);

    // poll_updates drains the queue immediately (messages already present)
    auto msgs = ch.poll_updates();
    REQUIRE(msgs.size() == 1);
    REQUIRE(msgs[0].sender      == "+1234567890");
    REQUIRE(msgs[0].content     == "Hello webhook!");
    REQUIRE(msgs[0].channel     == "whatsapp");
    REQUIRE(msgs[0].timestamp   == 1700000000);
    REQUIRE(msgs[0].reply_target.value_or("") == "+1234567890");
}

TEST_CASE("WhatsApp webhook: POST empty entry array returns 200 no messages", "[whatsapp_webhook]") {
    MockHttpClient http;
    auto cfg = make_webhook_config();
    cfg.webhook_secret = "";
    WhatsAppChannel ch(cfg, http);

    WebhookRequest req;
    req.method = "POST";
    req.path   = "/webhook";
    req.body   = R"({"entry":[]})";

    auto resp = ch.handle_webhook_request(req);
    REQUIRE(resp.status == 200);

    auto msgs = ch.poll_updates();
    REQUIRE(msgs.empty());
}

TEST_CASE("WhatsApp webhook: POST unauthorized sender not queued", "[whatsapp_webhook]") {
    MockHttpClient http;
    auto cfg = make_webhook_config();
    cfg.allow_from    = {"+9999999999"};  // only this number allowed
    cfg.webhook_secret = "";
    WhatsAppChannel ch(cfg, http);

    WebhookRequest req;
    req.method = "POST";
    req.path   = "/webhook";
    req.body   = kValidPayload;  // sender is +1234567890

    ch.handle_webhook_request(req);
    auto msgs = ch.poll_updates();
    REQUIRE(msgs.empty());
}

// ── Unsupported methods ───────────────────────────────────────────────────────

TEST_CASE("WhatsApp webhook: DELETE returns 405", "[whatsapp_webhook]") {
    MockHttpClient http;
    WhatsAppChannel ch(make_webhook_config(), http);

    WebhookRequest req;
    req.method = "DELETE";
    req.path   = "/webhook";

    auto resp = ch.handle_webhook_request(req);
    REQUIRE(resp.status == 405);
}

TEST_CASE("WhatsApp webhook: PUT returns 405", "[whatsapp_webhook]") {
    MockHttpClient http;
    WhatsAppChannel ch(make_webhook_config(), http);

    WebhookRequest req;
    req.method = "PUT";
    req.path   = "/webhook";

    auto resp = ch.handle_webhook_request(req);
    REQUIRE(resp.status == 405);
}

// ── supports_polling ──────────────────────────────────────────────────────────

TEST_CASE("WhatsApp webhook: supports_polling true when webhook_listen set", "[whatsapp_webhook]") {
    MockHttpClient http;
    auto cfg = make_webhook_config();
    cfg.webhook_listen = "127.0.0.1:8080";
    WhatsAppChannel ch(cfg, http);
    REQUIRE(ch.supports_polling());
}

TEST_CASE("WhatsApp webhook: supports_polling false without webhook_listen", "[whatsapp_webhook]") {
    MockHttpClient http;
    auto cfg = make_webhook_config();
    cfg.webhook_listen = "";
    WhatsAppChannel ch(cfg, http);
    REQUIRE_FALSE(ch.supports_polling());
}

// ── WebhookServer over real sockets ───────────────────────────────────────────
//
// Every test above calls a handler function directly, so the socket layer itself
// had no coverage at all. Streaming lives entirely in that layer — headers written
// before the body exists, incremental writes, a peer that vanishes mid-body — and
// none of it is observable from a handler-level test. These use a real loopback
// connection and assert on the bytes on the wire.

namespace {

// parse_listen_addr rejects port 0, so an ephemeral port cannot be requested.
// Scan a small range so a port left in TIME_WAIT by an earlier run, or a parallel
// job, does not fail the suite.
class TestServer {
public:
    TestServer(uint32_t max_body, const WebhookServer::Handler& handler,
               uint32_t max_connections = 1) {
        std::string error = "range exhausted";
        for (uint16_t p = 18730; p < 18760; ++p) {
            auto candidate = std::make_unique<WebhookServer>(
                "127.0.0.1:" + std::to_string(p), max_body, handler, max_connections);
            if (candidate->start(error)) {
                server_ = std::move(candidate);
                port_ = p;
                return;
            }
        }
        throw std::runtime_error("no free port for the test server: " + error);
    }

    ~TestServer() { if (server_) server_->stop(); }

    TestServer(const TestServer&) = delete;
    TestServer& operator=(const TestServer&) = delete;

    uint16_t port() const { return port_; }

    // Explicit, for the shutdown test — the destructor would hide the timing.
    void stop() { if (server_) server_->stop(); }

private:
    std::unique_ptr<WebhookServer> server_;
    uint16_t port_ = 0;
};

// Every client read is bounded, so a server that never writes fails the test on an
// assertion instead of hanging the suite forever.
int connect_to(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);

    struct timeval tv{6, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = ::inet_addr("127.0.0.1");
    REQUIRE(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    return fd;
}

void send_request(int fd, const std::string& raw) {
    REQUIRE(::send(fd, raw.data(), raw.size(), 0) == static_cast<ssize_t>(raw.size()));
}

// Read until `needle` appears, or the receive timeout expires.
std::string recv_until(int fd, const std::string& needle) {
    std::string acc;
    char buf[4096];
    while (acc.find(needle) == std::string::npos) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        acc.append(buf, static_cast<size_t>(n));
    }
    return acc;
}

std::string recv_until_close(int fd) {
    std::string acc;
    char buf[4096];
    for (;;) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        acc.append(buf, static_cast<size_t>(n));
    }
    return acc;
}

std::string to_lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

TEST_CASE("WebhookServer: a streamed chunk reaches the client before the handler returns",
          "[whatsapp_webhook]") {
    // The whole point of the feature, and the assertion is deliberately built so it
    // cannot pass under a buffering implementation: the producer blocks on the
    // client's acknowledgement of chunk one before writing chunk two. If the body
    // were held until the producer returned, the client would never see chunk one,
    // the wait would time out, and the test fails.
    std::mutex m;
    std::condition_variable cv;
    bool client_has_first = false;

    TestServer ts(1024, [&](const WebhookRequest&) {
        WebhookResponse r;
        r.content_type = "text/event-stream";
        r.stream = [&](const BodyWriter& write) {
            write("data: first\n\n");
            std::unique_lock<std::mutex> lk(m);
            cv.wait_for(lk, std::chrono::seconds(5), [&] { return client_has_first; });
            write("data: second\n\n");
        };
        return r;
    });

    int fd = connect_to(ts.port());
    send_request(fd, "POST /chat HTTP/1.1\r\nHost: t\r\nContent-Length: 0\r\n\r\n");

    std::string head = recv_until(fd, "data: first");
    REQUIRE(head.find("200 OK") != std::string::npos);
    REQUIRE(head.find("text/event-stream") != std::string::npos);
    // No Content-Length: the length is not knowable when the headers go out. A
    // streamed response that carried one would be malformed, not merely wasteful.
    REQUIRE(to_lower(head).find("content-length") == std::string::npos);
    // nginx buffers response bodies by default and Cache-Control does not change that,
    // so without this header small chunks would sit in the proxy's buffer and the
    // stream would arrive in one lump behind the documented reverse proxy — with no
    // error anywhere to explain why.
    REQUIRE(to_lower(head).find("x-accel-buffering: no") != std::string::npos);

    {
        std::lock_guard<std::mutex> lk(m);
        client_has_first = true;
    }
    cv.notify_all();

    REQUIRE(recv_until(fd, "data: second").find("data: second") != std::string::npos);
    ::close(fd);
}

TEST_CASE("WebhookServer: the body writer reports a peer that has gone away",
          "[whatsapp_webhook]") {
    // Without this signal a pod would keep generating tokens for a visitor who
    // closed the tab. It also covers the SIGPIPE hazard: writing to a hung-up peer
    // raises it, and if it were unhandled this test would kill the test process
    // rather than fail — so a crash here is a real result, not flakiness.
    std::atomic<bool> writer_said_gone{false};
    std::atomic<bool> producer_finished{false};

    TestServer ts(1024, [&](const WebhookRequest&) {
        WebhookResponse r;
        r.content_type = "text/event-stream";
        r.stream = [&](const BodyWriter& write) {
            // Big writes fill the socket buffer fast, so this ends promptly once the
            // client is gone instead of spinning on writes that still fit.
            const std::string filler(8192, 'x');
            for (int i = 0; i < 2000; ++i) {
                if (!write(filler)) {
                    writer_said_gone = true;
                    break;
                }
            }
            producer_finished = true;
        };
        return r;
    });

    int fd = connect_to(ts.port());
    send_request(fd, "POST /chat HTTP/1.1\r\nHost: t\r\nContent-Length: 0\r\n\r\n");
    REQUIRE_FALSE(recv_until(fd, "xxxx").empty());  // stream is running
    ::close(fd);                                     // vanish mid-body

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!producer_finished && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    REQUIRE(producer_finished.load());
    REQUIRE(writer_said_gone.load());
}

TEST_CASE("WebhookServer: a non-streaming response still carries Content-Length",
          "[whatsapp_webhook]") {
    // Regression guard for every existing caller: WhatsApp's handler returns a body,
    // and adding the streaming branch must not change how that is framed. Also the
    // first end-to-end test of the request-parse -> handler -> response path.
    TestServer ts(1024, [](const WebhookRequest& req) {
        WebhookResponse r;
        r.content_type = "application/json";
        r.body = req.path == "/webhook" ? R"({"status":"ok"})" : "wrong path";
        return r;
    });

    int fd = connect_to(ts.port());
    send_request(fd, "POST /webhook HTTP/1.1\r\nHost: t\r\nContent-Length: 0\r\n\r\n");

    std::string all = recv_until_close(fd);
    REQUIRE(all.find("Content-Length: 15") != std::string::npos);
    REQUIRE(all.find(R"({"status":"ok"})") != std::string::npos);
    ::close(fd);
}

TEST_CASE("WebhookServer: max_connections serves two clients at once",
          "[whatsapp_webhook]") {
    // Asserted on elapsed time, and that is not laziness: the obvious version of this
    // test — both clients get their body, and the handler saw arrived == 2 — passes
    // under serial handling too, because the second connection is simply served after
    // the first one's wait times out. Wall-clock is what actually separates the two.
    std::mutex m;
    std::condition_variable cv;
    int arrived = 0;

    TestServer ts(1024, [&](const WebhookRequest&) {
        {
            std::lock_guard<std::mutex> lk(m);
            ++arrived;
        }
        cv.notify_all();
        std::unique_lock<std::mutex> lk(m);
        cv.wait_for(lk, std::chrono::seconds(5), [&] { return arrived >= 2; });

        WebhookResponse r;
        r.content_type = "text/plain";
        r.body = "ok";
        return r;
    }, 4);

    const auto t0 = std::chrono::steady_clock::now();

    int a = connect_to(ts.port());
    send_request(a, "POST /a HTTP/1.1\r\nHost: t\r\nContent-Length: 0\r\n\r\n");
    int b = connect_to(ts.port());
    send_request(b, "POST /b HTTP/1.1\r\nHost: t\r\nContent-Length: 0\r\n\r\n");

    REQUIRE(recv_until_close(a).find("ok") != std::string::npos);
    REQUIRE(recv_until_close(b).find("ok") != std::string::npos);
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    REQUIRE(arrived == 2);
    // Serial handling costs the first handler's full 5 s timeout before the second is
    // even accepted; concurrent handling returns as soon as both are inside.
    REQUIRE(elapsed < std::chrono::seconds(3));

    ::close(a);
    ::close(b);
}

TEST_CASE("WebhookServer: stop() is not held open by a live stream",
          "[whatsapp_webhook]") {
    // The shutdown hazard: with connection threads detached and holding `this`, stop()
    // must wait for them — but a healthy client keeps every write succeeding, so
    // without a cancellation signal the wait lasts as long as the client cares to
    // listen. The writer reporting false on shutdown is what bounds it.
    std::atomic<bool> writer_reported_stop{false};
    std::atomic<bool> producer_done{false};

    TestServer ts(1024, [&](const WebhookRequest&) {
        WebhookResponse r;
        r.content_type = "text/event-stream";
        r.stream = [&](const BodyWriter& write) {
            // 2000 ticks is ~20 s if nothing interrupts it.
            for (int i = 0; i < 2000; ++i) {
                if (!write("data: tick\n\n")) {
                    writer_reported_stop = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            producer_done = true;
        };
        return r;
    }, 4);

    int fd = connect_to(ts.port());
    send_request(fd, "POST /chat HTTP/1.1\r\nHost: t\r\nContent-Length: 0\r\n\r\n");
    REQUIRE_FALSE(recv_until(fd, "data: tick").empty());  // the stream is live

    const auto t0 = std::chrono::steady_clock::now();
    ts.stop();
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    // Ordering matters here: stop() returning *before* the producer finished would mean
    // a detached thread still holding a destroyed server.
    REQUIRE(producer_done.load());
    REQUIRE(writer_reported_stop.load());
    REQUIRE(elapsed < std::chrono::seconds(5));

    ::close(fd);
}
