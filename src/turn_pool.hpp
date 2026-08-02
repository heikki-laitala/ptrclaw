#pragma once
#include "event.hpp"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ptrclaw {

class EventBus;

// TurnPool runs agent turns for different sessions in parallel.
//
// Without it PtrClaw executes one turn at a time for the whole process: the poll
// loop publishes MessageReceivedEvent synchronously, and EventBus::publish runs
// its handlers inline, so a turn completes before the next message is dequeued.
//
// ⚠ Sharding by session id is the whole safety argument. submit() routes an event
// to worker `fnv1a(session_id) % workers`, so a session always lands on the same
// thread. Two invariants follow, and everything downstream depends on them:
//
//   1. Turns for one session run in arrival order, never concurrently.
//   2. No two threads ever touch one Agent — which is what lets Agent, and the
//      per-session ToolManager, stay free of synchronisation.
//
// Sessions do share the EventBus, but Agent and ToolManager both filter incoming
// events on session_id, so a turn on one thread never reaches another session's
// handlers.
//
// With `workers <= 1` no thread is created and submit() publishes inline on the
// caller's thread — byte-for-byte today's behaviour, which is why the `workers`
// config key defaults to 1.
class TurnPool {
public:
    TurnPool(EventBus& bus, uint32_t workers);
    ~TurnPool();

    TurnPool(const TurnPool&) = delete;
    TurnPool& operator=(const TurnPool&) = delete;

    // Queue a turn on the shard owning ev.session_id. Returns immediately unless
    // the pool is running inline (workers <= 1), or the shard's queue is full —
    // see kMaxQueuedPerShard.
    void submit(MessageReceivedEvent ev);

    // True when no shard has queued events or a turn in flight. Does not block.
    //
    // Like drain(), only meaningful from the sole submitting thread: nothing else
    // can enqueue, so a pool that reads as idle stays idle until that thread
    // submits again.
    bool idle() const;

    // Block until every shard is idle: no queued events and no turn in flight.
    //
    // Only meaningful when called from the sole submitting thread — otherwise
    // another thread could enqueue work into a shard already checked. In PtrClaw
    // that thread is the poll loop, which is the only caller of submit().
    //
    // Callers on a latency path should gate on idle() first: this waits out the
    // whole queue, not just the turn in flight, and the poll loop is not reading
    // the channel while it waits.
    void drain();

    // Stop the workers and join them. Queued turns that have not started are
    // dropped; a turn already running is waited for. Idempotent.
    void stop();

    // Number of worker threads; 0 when running inline.
    uint32_t workers() const { return static_cast<uint32_t>(shards_.size()); }

    // Which shard a session id maps to. Exposed for tests.
    static size_t shard_for(const std::string& session_id, uint32_t workers);

private:
    // Queue depth per shard, above which submit() blocks.
    //
    // Back-pressure, and the pool has to supply it: publishing inline used to
    // bound intake to one turn at a time, so an unbounded queue would let a
    // channel with no per-session gate — Telegram and WhatsApp both lack
    // HttpChannel's 409 — grow without limit while one turn runs, each entry
    // holding a whole ChannelMessage and its history window. Blocking the poll
    // thread is the honest response: it is what the old inline path did.
    static constexpr size_t kMaxQueuedPerShard = 64;

    struct Shard {
        std::mutex                        mutex;
        std::condition_variable           work_cv;   // worker waits for a turn
        std::condition_variable           space_cv;  // submit() waits for room
        std::condition_variable           idle_cv;   // drain() waits for quiet
        std::deque<MessageReceivedEvent>  queue;
        bool                              busy = false;
    };

    void run(size_t index);

    EventBus&                           bus_;
    // unique_ptr because Shard holds a mutex and a condition_variable, neither of
    // which is movable, so the vector cannot hold them by value.
    std::vector<std::unique_ptr<Shard>> shards_;
    std::vector<std::thread>            threads_;
    std::atomic<bool>                   stopping_{false};
};

} // namespace ptrclaw
