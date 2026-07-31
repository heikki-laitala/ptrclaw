#pragma once
#include "../channel.hpp"
#include "webhook_server.hpp"
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ptrclaw {

class EventBus;

struct HttpChannelConfig {
    std::string listen = "127.0.0.1:8080";
    // Optional shared secret, required as "Authorization: Bearer <secret>" when set.
    // Empty means no authentication — only safe on a loopback or in-cluster address.
    std::string secret;
    uint32_t    max_body = 65536;
    // Concurrent connections. Turns still run one at a time (see the class comment), so
    // this governs how many callers can be *waiting* rather than how many are served.
    uint32_t    max_connections = 8;
    // How long a single turn may take before the stream is closed with an error. Without
    // it a provider that never answers would hold a connection open forever.
    uint32_t    turn_timeout_seconds = 120;
};

// HttpChannel serves chat over HTTP with Server-Sent Events, for a front end that owns
// the conversation and pushes it with every message.
//
//   POST /chat  {"session": "...", "message": "...", "history": [{role, content}, ...]}
//   → text/event-stream:  event: token / data: {"delta": "..."}
//                         event: done  / data: {"content": "..."}
//                         event: error / data: {"message": "..."}
//   GET /healthz → 200, for container probes
//
// `history` is optional and, when present, *replaces* the session's own history for that
// turn (ChannelMessage::history) — so the caller can be the single source of truth and the
// agent stays a stateless consumer.
//
// ⚠ Threading, and it is the whole design constraint. A request arrives on a
// WebhookServer connection thread, but the turn runs on the poll-loop thread, and
// `EventBus::publish` is synchronous — so PtrClaw executes **one turn at a time for the
// whole process**. Several callers can therefore be connected and streaming, while their
// turns queue behind one another. Nothing here can change that; it lives in the poll loop.
//
// Deltas cross between the two threads through `turns_`: the poll thread appends, the
// connection thread drains and writes. No Agent state is touched from a connection
// thread, which matters because Agent has no synchronisation of its own.
class HttpChannel : public Channel {
public:
    explicit HttpChannel(HttpChannelConfig config);
    ~HttpChannel() override;

    // ── Channel interface ────────────────────────────────────────
    std::string channel_name() const override { return "http"; }
    bool health_check() override;
    bool supports_polling() const override;
    void set_event_bus(EventBus* bus) override;
    void initialize() override;
    std::vector<ChannelMessage> poll_updates() override;
    void send_message(const std::string& target, const std::string& message) override;

    // Handle a parsed request without a socket. Public for tests: the routing,
    // authentication and body validation are worth testing without a TCP connection,
    // while the streaming half needs a real one.
    WebhookResponse handle_request(const WebhookRequest& req);

private:
    // One in-flight turn. `deltas` is a queue rather than a string because the connection
    // thread must be able to emit each token as it arrives.
    struct Turn {
        std::deque<std::string> deltas;
        std::string             final_content;
        std::string             error;
        bool                    done = false;
    };

    void stream_turn(const std::string& session, const BodyWriter& write);
    void append_delta(const std::string& session, const std::string& delta);
    void fail_turn(const std::string& session, const std::string& error);

    HttpChannelConfig              config_;
    EventBus*                      bus_ = nullptr;
    std::unique_ptr<WebhookServer> server_;

    std::mutex                  inbound_mutex_;
    std::vector<ChannelMessage> inbound_queue_;

    std::mutex                                     turn_mutex_;
    std::condition_variable                        turn_cv_;
    std::unordered_map<std::string, Turn>          turns_;
};

} // namespace ptrclaw
