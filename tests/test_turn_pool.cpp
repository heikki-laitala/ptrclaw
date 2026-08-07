#include <catch2/catch_test_macros.hpp>
#include "turn_pool.hpp"
#include "config.hpp"
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

// ── Threads exist only while turns do ───────────────────────────
//
// These replace two tests that asserted the hash mapped a session to a fixed shard.
// There are no shards now; what those tests were really protecting — one turn per
// session at a time, in order — is asserted directly further down.

TEST_CASE("TurnPool: a pool with nothing to do holds no threads", "[turn_pool]") {
    // The point of the change: a pod configured for a thousand concurrent turns pays
    // for none of them while idle.
    EventBus bus;
    TurnPool pool(bus, 1024);
    REQUIRE(pool.workers() == 1024);
    REQUIRE(pool.in_flight() == 0);
    REQUIRE(pool.idle());
}

TEST_CASE("TurnPool: threads appear for work and leave when it is done",
          "[turn_pool]") {
    EventBus bus;
    std::mutex m;
    std::condition_variable cv;
    bool release = false;
    std::atomic<int> started{0};

    subscribe<MessageReceivedEvent>(bus, [&](const MessageReceivedEvent&) {
        ++started;
        std::unique_lock<std::mutex> lock(m);
        cv.wait(lock, [&] { return release; });
    });

    TurnPool pool(bus, 8);
    pool.submit(make_event("s1"));
    pool.submit(make_event("s2"));

    // Both turns are held inside the handler, so both slots are occupied.
    for (int i = 0; i < 200 && started.load() < 2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(started.load() == 2);
    REQUIRE(pool.in_flight() == 2);

    { std::lock_guard<std::mutex> lock(m); release = true; }
    cv.notify_all();
    pool.drain();

    // ...and released once the turns finish, rather than parked for the next one.
    REQUIRE(pool.in_flight() == 0);
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

    // One capture, not four: a closure over more than a couple of references
    // outgrows std::function's small buffer and heap-allocates, which the static
    // analyzer reports as a leak through EventBus::subscribe.
    struct Barrier {
        std::mutex m;
        std::condition_variable cv;
        int arrived = 0;
        std::atomic<int> both_saw{0};
    } barrier;

    subscribe<MessageReceivedEvent>(bus, [&barrier](const MessageReceivedEvent&) {
        std::unique_lock<std::mutex> lock(barrier.m);
        ++barrier.arrived;
        barrier.cv.notify_all();
        if (barrier.cv.wait_for(lock, std::chrono::seconds(5),
                                [&barrier] { return barrier.arrived == 2; })) {
            ++barrier.both_saw;
        }
    });

    // Any two distinct ids will do now: parallelism no longer depends on where a hash
    // puts them, which is the point of dropping the shards.
    std::string a = "session-a";
    std::string b = "session-b";

    pool.submit(make_event(a));
    pool.submit(make_event(b));
    pool.drain();

    REQUIRE(barrier.both_saw.load() == 2);
}

// ── drain() ─────────────────────────────────────────────────────

TEST_CASE("TurnPool: a full session queue applies back-pressure to the submitter",
          "[turn_pool]") {
    // Publishing inline used to bound intake to one turn at a time. An unbounded
    // queue would let a channel with no per-session gate grow without limit while
    // one turn runs, each entry holding a whole ChannelMessage.
    EventBus bus;
    TurnPool pool(bus, 2);

    std::atomic<bool> release{false};
    std::atomic<int> started{0};
    subscribe<MessageReceivedEvent>(bus, [&](const MessageReceivedEvent&) {
        ++started;
        while (!release.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    // All on one shard, so they queue behind the blocked turn.
    const std::string session = "one-session";
    std::atomic<int> submitted{0};
    std::thread producer([&] {
        for (int i = 0; i < 500; ++i) {
            pool.submit(make_event(session));
            ++submitted;
        }
    });

    REQUIRE(wait_for([&] { return started.load() == 1; }));
    // Give the producer every chance to run away if the queue were unbounded.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // One in flight plus a bounded queue — nowhere near all 500.
    REQUIRE(submitted.load() < 500);
    REQUIRE(submitted.load() <= 1 + 64 + 1);

    release = true;
    producer.join();
    pool.drain();
    REQUIRE(submitted.load() == 500);
}

TEST_CASE("TurnPool: stop releases a submitter blocked on a full queue",
          "[turn_pool]") {
    // Otherwise shutdown deadlocks: stop() joins the workers while the poll
    // thread is still parked waiting for room that will never come.
    EventBus bus;
    auto pool = std::make_unique<TurnPool>(bus, 2);

    std::atomic<bool> release{false};
    subscribe<MessageReceivedEvent>(bus, [&](const MessageReceivedEvent&) {
        while (!release.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    std::atomic<bool> producer_done{false};
    std::thread producer([&] {
        for (int i = 0; i < 500; ++i) pool->submit(make_event("one-session"));
        producer_done = true;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    REQUIRE_FALSE(producer_done.load());  // parked on a full shard

    release = true;
    pool->stop();
    producer.join();
    REQUIRE(producer_done.load());
}

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

TEST_CASE("TurnPool: the configured ceiling is constructible and joins cleanly",
          "[turn_pool]") {
    // kMaxWorkers was raised from 64 to 1024, which is only meaningful if a pool that size
    // starts and shuts down. Construction that throws part-way is handled by stopping and
    // joining what exists — unwinding with joinable threads alive calls std::terminate —
    // but that path needs a thread limit to trip and is not reachable from a unit test, so
    // this pins the reachable half.
    EventBus bus;
    {
        TurnPool pool(bus, kMaxWorkers);
        REQUIRE(pool.workers() == kMaxWorkers);
        REQUIRE(pool.idle());
    }  // destructor joins every worker; a leak or a missed join hangs or crashes here
    SUCCEED("pool of kMaxWorkers workers constructed and destroyed");
}

TEST_CASE("TurnPool: destroying a pool the instant its turns finish is safe",
          "[turn_pool]") {
    // The workers are detached, so stop() waiting for in_flight_ to reach zero *is* the
    // join. If a worker published that zero before its last touch of the pool, stop() could
    // return and the pool be destroyed while that worker was still calling notify on its
    // condition variables.
    //
    // Probabilistic by nature — it races destruction against worker exit rather than
    // proving the ordering — so it runs the window many times and is worth far more under
    // a sanitiser than on its own.
    EventBus bus;
    std::atomic<int> ran{0};
    subscribe<MessageReceivedEvent>(bus, [&ran](const MessageReceivedEvent&) { ++ran; });

    for (int round = 0; round < 300; ++round) {
        TurnPool pool(bus, 4);
        pool.submit(make_event("a"));
        pool.submit(make_event("b"));
        pool.submit(make_event("c"));
        pool.submit(make_event("d"));
        // No drain(): the destructor runs while those turns are finishing, which is the
        // interleaving being tested.
    }
    SUCCEED("300 pools destroyed while their turns were completing");
}
