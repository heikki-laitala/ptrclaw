#pragma once
#include "event.hpp"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

namespace ptrclaw {

class EventBus;

// TurnPool runs agent turns for different sessions in parallel.
//
// Without it PtrClaw executes one turn at a time for the whole process: the poll
// loop publishes MessageReceivedEvent synchronously, and EventBus::publish runs
// its handlers inline, so a turn completes before the next message is dequeued.
//
// ⚠ Per-session serialisation is the whole safety argument. A session runs at most
// one turn at a time, and its next queued turn is picked up by the thread that
// just finished the previous one. Two invariants follow, and everything
// downstream depends on them:
//
//   1. Turns for one session run in arrival order, never concurrently.
//   2. No two threads ever touch one Agent at once — which is what lets Agent, and
//      the per-session ToolManager, stay free of synchronisation.
//
// The second invariant is subtler than it was. Turns for one session may now run
// on *different* threads over their lifetime, where sharding pinned them to one.
// That is safe because every handoff passes through mutex_: the finishing thread
// releases it after the previous turn's writes, and the next acquires it before
// the following turn's reads, which is the happens-before edge Agent needs. What
// would break the invariant is two turns for one session running at once, and
// `running` prevents exactly that.
//
// Sessions do share the EventBus, but Agent and ToolManager both filter incoming
// events on session_id, so a turn on one thread never reaches another session's
// handlers.
//
// Threads are created when a turn needs one and exit when its session runs dry,
// rather than being started up front and parked. Idle cost is then independent of
// the ceiling: a pod able to serve a thousand concurrent turns holds no threads
// while nothing is happening. Creation costs ~10 us against a turn measured in
// seconds. It also removes the tail that hash-sharding produced — work no longer
// lands in a fixed bin, so a burst is not paced by whichever bin drew the most.
//
// With `workers <= 1` no thread is created and submit() publishes inline on the
// caller's thread — byte-for-byte the personal agent's behaviour, which is why
// the `workers` config key defaults to 1 outside a serving build.
class TurnPool {
public:
    // `workers` is the ceiling on turns running at once, not a thread count: no
    // thread exists until a turn needs one.
    TurnPool(EventBus& bus, uint32_t workers);
    ~TurnPool();

    TurnPool(const TurnPool&) = delete;
    TurnPool& operator=(const TurnPool&) = delete;

    // Run a turn for ev.session_id, or queue it behind that session's current one.
    // Returns immediately unless the pool is running inline (workers <= 1), the
    // session's queue is full — see kMaxQueuedPerSession — or every slot is taken.
    void submit(MessageReceivedEvent ev);

    // True when nothing is queued and no turn is in flight. Does not block.
    //
    // Like drain(), only meaningful from the sole submitting thread: nothing else
    // can enqueue, so a pool that reads as idle stays idle until that thread
    // submits again.
    bool idle() const;

    // Block until nothing is queued and no turn is in flight.
    //
    // Only meaningful when called from the sole submitting thread — otherwise
    // another thread could enqueue work after the check. In PtrClaw that thread is
    // the poll loop, which is the only caller of submit().
    //
    // Callers on a latency path should gate on idle() first: this waits out the
    // whole queue, not just the turns in flight, and the poll loop is not reading
    // the channel while it waits.
    void drain();

    // Stop accepting work and wait for running turns to finish. Queued turns that
    // have not started are dropped; a turn already running is waited for.
    // Idempotent.
    void stop();

    // Ceiling on concurrent turns; 0 when running inline.
    uint32_t workers() const { return max_concurrent_; }

    // Turns in flight right now. Exposed for tests.
    uint32_t in_flight() const;

private:
    // Queue depth per session, above which submit() blocks.
    //
    // Back-pressure, and the pool has to supply it: publishing inline used to
    // bound intake to one turn at a time, so an unbounded queue would let a
    // channel with no per-session gate — Telegram and WhatsApp both lack
    // HttpChannel's 409 — grow without limit while one turn runs, each entry
    // holding a whole ChannelMessage and its history window. Blocking the poll
    // thread is the honest response: it is what the old inline path did.
    static constexpr size_t kMaxQueuedPerSession = 64;

    struct Session {
        std::deque<MessageReceivedEvent> queue;
        bool                             running = false;
    };

    // Runs `first` and then whatever else arrives for that session, exiting when
    // the session runs dry. Owns one of the max_concurrent_ slots throughout.
    void run_session(const std::string& session_id, MessageReceivedEvent first);

    // Starts run_session on a new thread, or runs it here if no thread can be
    // created. Caller must NOT hold mutex_.
    void start(std::string session_id, MessageReceivedEvent ev);

    // Caller must hold mutex_.
    bool quiet() const;

    EventBus&                                  bus_;
    uint32_t                                   max_concurrent_ = 0;

    mutable std::mutex                         mutex_;
    // A submitter waits here for queue room or a free slot.
    std::condition_variable                    space_cv_;
    // drain() and stop() wait here for the pool to fall quiet.
    std::condition_variable                    quiet_cv_;
    std::unordered_map<std::string, Session>   sessions_;
    // Sessions with a turn running, which is also the number of live threads.
    uint32_t                                   in_flight_ = 0;
    std::atomic<bool>                          stopping_{false};
};

} // namespace ptrclaw
