#include <catch2/catch_test_macros.hpp>
#include "turn_pool.hpp"
#include "event.hpp"
#include "event_bus.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace ptrclaw;

namespace {

MessageReceivedEvent make_event(const std::string& session,
                                const std::string& content = "hi") {
    MessageReceivedEvent ev;
    ev.session_id = session;
    ev.message.sender = session;
    ev.message.content = content;
    return ev;
}

// Wait for `pred` up to `timeout`, so a failing test reports rather than hangs.
template <typename Pred>
bool wait_for(Pred pred, std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return pred();
}

} // namespace

// ── Sharding ────────────────────────────────────────────────────

TEST_CASE("TurnPool: a session id always maps to the same shard", "[turn_pool]") {
    // The whole safety argument rests on this: one session, one thread, so no two
    // threads ever touch one Agent.
    for (uint32_t workers : {2u, 4u, 8u, 17u}) {
        size_t first = TurnPool::shard_for("session-abc", workers);
        for (int i = 0; i < 100; ++i) {
            REQUIRE(TurnPool::shard_for("session-abc", workers) == first);
        }
        REQUIRE(first < workers);
    }
}

TEST_CASE("TurnPool: sessions spread across shards", "[turn_pool]") {
    std::set<size_t> shards;
    for (int i = 0; i < 200; ++i) {
        shards.insert(TurnPool::shard_for("session-" + std::to_string(i), 4));
    }
    REQUIRE(shards.size() == 4);
}

// ── Inline mode ─────────────────────────────────────────────────

TEST_CASE("TurnPool: workers<=1 publishes inline on the caller's thread",
          "[turn_pool]") {
    // The default. It must stay byte-for-byte the old behaviour: no thread, and
    // submit() does not return until the turn is done.
    EventBus bus;
    TurnPool pool(bus, 1);
    REQUIRE(pool.workers() == 0);

    std::thread::id handler_thread;
    bool ran = false;
    subscribe<MessageReceivedEvent>(bus, [&](const MessageReceivedEvent&) {
        handler_thread = std::this_thread::get_id();
        ran = true;
    });

    pool.submit(make_event("s1"));

    REQUIRE(ran);  // synchronous — no waiting needed
    REQUIRE(handler_thread == std::this_thread::get_id());
}

// ── Ordering and parallelism ────────────────────────────────────

TEST_CASE("TurnPool: turns for one session run in submission order",
          "[turn_pool]") {
    EventBus bus;
    TurnPool pool(bus, 4);

    std::mutex m;
    std::vector<std::string> seen;
    subscribe<MessageReceivedEvent>(bus, [&](const MessageReceivedEvent& ev) {
        std::lock_guard<std::mutex> lock(m);
        seen.push_back(ev.message.content);
    });

    for (int i = 0; i < 20; ++i) {
        pool.submit(make_event("same-session", std::to_string(i)));
    }
    pool.drain();

    std::lock_guard<std::mutex> lock(m);
    REQUIRE(seen.size() == 20);
    for (int i = 0; i < 20; ++i) {
        REQUIRE(seen[static_cast<size_t>(i)] == std::to_string(i));
    }
}

TEST_CASE("TurnPool: turns for one session never overlap", "[turn_pool]") {
    EventBus bus;
    TurnPool pool(bus, 4);

    std::atomic<int> concurrent{0};
    std::atomic<int> max_concurrent{0};
    subscribe<MessageReceivedEvent>(bus, [&](const MessageReceivedEvent&) {
        int now = ++concurrent;
        int prev = max_concurrent.load();
        while (now > prev && !max_concurrent.compare_exchange_weak(prev, now)) {}
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        --concurrent;
    });

    for (int i = 0; i < 8; ++i) pool.submit(make_event("one-session"));
    pool.drain();

    REQUIRE(max_concurrent.load() == 1);
}

TEST_CASE("TurnPool: turns for different sessions run in parallel",
          "[turn_pool]") {
    // The point of the whole change. Two sessions, each blocking until the other
    // has arrived — serial execution would deadlock, so the barrier completing is
    // the proof.
    EventBus bus;
    TurnPool pool(bus, 4);

    std::mutex m;
    std::condition_variable cv;
    int arrived = 0;
    std::atomic<int> both_saw{0};

    subscribe<MessageReceivedEvent>(bus, [&](const MessageReceivedEvent&) {
        std::unique_lock<std::mutex> lock(m);
        ++arrived;
        cv.notify_all();
        if (cv.wait_for(lock, std::chrono::seconds(5), [&] { return arrived == 2; })) {
            ++both_saw;
        }
    });

    // Two ids the hash puts on different shards — asserted, not assumed.
    std::string a = "session-a";
    std::string b = "session-b";
    REQUIRE(TurnPool::shard_for(a, 4) != TurnPool::shard_for(b, 4));

    pool.submit(make_event(a));
    pool.submit(make_event(b));
    pool.drain();

    REQUIRE(both_saw.load() == 2);
}

// ── drain() ─────────────────────────────────────────────────────

TEST_CASE("TurnPool: idle reports work in flight without blocking",
          "[turn_pool]") {
    // The poll loop gates eviction on this so it does not block reading the
    // channel for the length of the slowest turn.
    EventBus bus;
    TurnPool pool(bus, 2);

    REQUIRE(pool.idle());

    std::atomic<bool> release{false};
    std::atomic<bool> started{false};
    subscribe<MessageReceivedEvent>(bus, [&](const MessageReceivedEvent&) {
        started = true;
        while (!release.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    pool.submit(make_event("s1"));
    REQUIRE(wait_for([&] { return started.load(); }));

    auto before = std::chrono::steady_clock::now();
    bool busy = !pool.idle();
    auto elapsed = std::chrono::steady_clock::now() - before;

    REQUIRE(busy);
    REQUIRE(elapsed < std::chrono::milliseconds(100));  // did not block

    release = true;
    pool.drain();
    REQUIRE(pool.idle());
}

TEST_CASE("TurnPool: idle is true for an inline pool", "[turn_pool]") {
    EventBus bus;
    TurnPool pool(bus, 1);
    REQUIRE(pool.idle());
}

TEST_CASE("TurnPool: drain waits for queued and in-flight turns", "[turn_pool]") {
    // drain() is what makes session eviction safe: after it returns, no handler is
    // running anywhere, so freeing a session cannot pull the rug from under one.
    EventBus bus;
    TurnPool pool(bus, 4);

    std::atomic<int> completed{0};
    subscribe<MessageReceivedEvent>(bus, [&](const MessageReceivedEvent&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        ++completed;
    });

    for (int i = 0; i < 12; ++i) {
        pool.submit(make_event("session-" + std::to_string(i)));
    }
    pool.drain();

    REQUIRE(completed.load() == 12);
}

// ── Failure and shutdown ────────────────────────────────────────

TEST_CASE("TurnPool: a throwing turn does not take the process down",
          "[turn_pool]") {
    // Inline, an exception reached main()'s handler and exited. On a worker thread
    // an uncaught one is std::terminate, killing every other session with it.
    EventBus bus;
    TurnPool pool(bus, 2);

    std::atomic<int> ran{0};
    subscribe<MessageReceivedEvent>(bus, [&](const MessageReceivedEvent& ev) {
        ++ran;
        if (ev.message.content == "boom") throw std::runtime_error("boom");
    });

    pool.submit(make_event("session-boom", "boom"));
    pool.drain();
    pool.submit(make_event("session-boom", "fine"));
    pool.drain();

    REQUIRE(ran.load() == 2);  // the worker survived and took the next turn
}

TEST_CASE("TurnPool: stop joins workers and is idempotent", "[turn_pool]") {
    EventBus bus;
    TurnPool pool(bus, 4);

    std::atomic<int> ran{0};
    subscribe<MessageReceivedEvent>(bus, [&](const MessageReceivedEvent&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        ++ran;
    });

    pool.submit(make_event("s1"));
    REQUIRE(wait_for([&] { return ran.load() == 1; }));

    pool.stop();
    pool.stop();  // idempotent

    // Submissions after stop are dropped rather than queued forever.
    pool.submit(make_event("s2"));
    REQUIRE(ran.load() == 1);
}

TEST_CASE("TurnPool: stop does not wait out a backlog", "[turn_pool]") {
    // Shutdown must be prompt. Queued-but-unstarted turns are dropped; only the
    // turn already running is waited for.
    EventBus bus;
    TurnPool pool(bus, 1 + 1);  // 2 workers

    std::atomic<int> ran{0};
    subscribe<MessageReceivedEvent>(bus, [&](const MessageReceivedEvent&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        ++ran;
    });

    // All on one shard, so they queue behind one another.
    for (int i = 0; i < 50; ++i) pool.submit(make_event("same"));

    auto start = std::chrono::steady_clock::now();
    pool.stop();
    auto elapsed = std::chrono::steady_clock::now() - start;

    REQUIRE(ran.load() < 50);
    // 50 x 20ms would be a full second; one in-flight turn is ~20ms.
    REQUIRE(elapsed < std::chrono::milliseconds(500));
}
