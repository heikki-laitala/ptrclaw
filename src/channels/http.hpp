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
    // Concurrent connections, not concurrent turns — past this the acceptor stops
    // accepting and the kernel backlog queues callers. It has to stay above `workers`, or
    // callers that could have been served are waiting on a connection instead. A serving
    // build defaults above 2x its own worker default; each connection is a detached thread
    // and costs little while it waits.
#ifdef PTRCLAW_HAS_SERVING
    uint32_t    max_connections = 32;
#else
    uint32_t    max_connections = 8;
#endif
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
        // Pre-rendered SSE frames waiting to go out, in the order they were produced.
        // Rendered at enqueue rather than at write so tokens and tool events can share one
        // queue: their relative order is what tells a caller which tool a sentence came
        // from, and two queues could not preserve it.
        std::deque<std::string> pending;
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
        // One condition variable per turn, not one for the channel.
        //
        // A single shared cv meant every producer had to notify_all(): a token for one
        // session woke every connection thread in the process, each to re-acquire
        // turn_mutex_, find the event was not theirs, and sleep again. Under load that is
        // O(waiters) wakeups per token — measured at 500 concurrent turns, 315 of 449
        // threads sat blocked on that mutex while only 65 were in a provider call.
        //
        // Turns are held by shared_ptr so this outlives its map entry: a waiter keeps its
        // own reference, and erasing the session's entry cannot destroy the cv underneath
        // a thread waiting on it.
        std::condition_variable               cv;
        // Set when the turn leaves the map, so a waiter can tell "my turn was taken away"
        // from a spurious wake without consulting the map at all.
        bool                                  detached = false;
    };

    using TurnRef = std::shared_ptr<Turn>;

    void stream_turn(const std::string& session, const TurnRef& turn,
                     const BodyWriter& write);
    // Detaches and erases the session's turn if it is still the one identified by `seq`,
    // waking whoever waits on it. Caller must hold turn_mutex_.
    void erase_turn(const std::string& session, uint64_t seq);
    // Ends every in-flight turn with an error and wakes the threads writing them.
    void release_pending_turns(const std::string& reason);
    void append_delta(const std::string& session, const std::string& delta);
    // Queues an already-rendered frame on the session's turn, if one is in flight.
    void enqueue_frame(const std::string& session, const std::string& frame);
    void fail_turn(const std::string& session, const std::string& error);

    HttpChannelConfig              config_;
    EventBus*                      bus_ = nullptr;
    std::unique_ptr<WebhookServer> server_;

    std::mutex                  inbound_mutex_;
    std::condition_variable     inbound_cv_;
    std::vector<ChannelMessage> inbound_queue_;

    // Still one mutex: the map is small and every critical section under it is a handful
    // of instructions. What changed is who gets woken — see Turn::cv.
    std::mutex                                     turn_mutex_;
    std::unordered_map<std::string, TurnRef>       turns_;
    uint64_t                                       next_turn_seq_ = 1;
};

} // namespace ptrclaw
