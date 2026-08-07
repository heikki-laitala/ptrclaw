#pragma once
#include "../channel.hpp"
#include "webhook_server.hpp"
#include <chrono>
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
    // Concurrent connections. Turns run in parallel only up to the `workers` config key
    // (see the class comment), so above that this governs how many callers can be
    // *waiting* rather than how many are served.
    uint32_t    max_connections = 8;
    // How long a single turn may take before the stream is closed with an error. Without
    // it a provider that never answers would hold a connection open forever.
    uint32_t    turn_timeout_seconds = 120;
    // Whether a request may omit "session" and have one generated, announced to the caller
    // as the first SSE frame. Mirrors Config::serving.generate_session_ids, which the
    // channel registrar copies in. Off by default: an id is a routing key the caller has
    // always supplied, and inventing one silently would hide a client bug.
    bool        generate_session_ids = false;
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
// WebhookServer connection thread, but the turn runs elsewhere: the poll loop hands it
// to TurnPool, which shards by session id. So turns for *different* sessions run in
// parallel (up to the `workers` config key, 1 by default — at 1 the pool dispatches
// inline and the process still runs one turn at a time), while turns for *one* session
// are serialised on that session's worker. The 409 above is the other half of that: it
// refuses a second concurrent turn on a session rather than queueing it, because two
// turns interleaving over one Agent and one history is not a conversation.
//
// Deltas cross between the two threads through `turns_`: the worker running the turn
// appends, the connection thread drains and writes. No Agent state is touched from a
// connection thread, which matters because Agent has no synchronisation of its own.
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
        // Only used to recognise an abandoned turn. WebhookServer can return before
        // invoking the producer (if the response headers fail to send), which would leave
        // an entry nothing will ever clear — and with one turn per session enforced, that
        // would wedge the session permanently.
        std::chrono::steady_clock::time_point started{};
        // Distinguishes this turn from the next one under the same session id. The id alone
        // is not enough: a turn can be taken away while its connection thread is parked —
        // by POST /session/end, or by the stale-turn path — and the id reused before that
        // thread wakes. Finding *an* entry would then stream one client's tokens to another
        // and let the dead stream's cleanup erase a live turn.
        uint64_t                              seq = 0;
    };

    void stream_turn(const std::string& session, uint64_t seq, const BodyWriter& write);
    // Erases the session's turn only if it is still the one identified by `seq`.
    // Caller must hold turn_mutex_.
    void erase_turn(const std::string& session, uint64_t seq);
    // Ends every in-flight turn with an error and wakes the threads writing them.
    void release_pending_turns(const std::string& reason);
    void append_delta(const std::string& session, const std::string& delta);
    void fail_turn(const std::string& session, const std::string& error);

    HttpChannelConfig              config_;
    EventBus*                      bus_ = nullptr;
    std::unique_ptr<WebhookServer> server_;

    std::mutex                  inbound_mutex_;
    std::condition_variable     inbound_cv_;
    std::vector<ChannelMessage> inbound_queue_;

    std::mutex                                     turn_mutex_;
    std::condition_variable                        turn_cv_;
    std::unordered_map<std::string, Turn>          turns_;
    uint64_t                                       next_turn_seq_ = 1;
};

} // namespace ptrclaw
