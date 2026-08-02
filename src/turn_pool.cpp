#include "turn_pool.hpp"
#include "event_bus.hpp"
#include "util.hpp"
#include <iostream>

namespace ptrclaw {

TurnPool::TurnPool(EventBus& bus, uint32_t workers) : bus_(bus) {
    if (workers <= 1) return;  // inline mode — no shards, no threads

    shards_.reserve(workers);
    for (uint32_t i = 0; i < workers; ++i) {
        shards_.push_back(std::make_unique<Shard>());
    }

    threads_.reserve(workers);
    for (uint32_t i = 0; i < workers; ++i) {
        threads_.emplace_back([this, i] { run(i); });
    }
}

TurnPool::~TurnPool() {
    stop();
}

size_t TurnPool::shard_for(const std::string& session_id, uint32_t workers) {
    if (workers <= 1) return 0;
    return static_cast<size_t>(fnv1a(session_id) % workers);
}

void TurnPool::submit(MessageReceivedEvent ev) {
    if (shards_.empty()) {
        bus_.publish(ev);
        return;
    }

    Shard& shard = *shards_[shard_for(ev.session_id, workers())];
    {
        std::lock_guard<std::mutex> lock(shard.mutex);
        if (stopping_.load()) return;
        shard.queue.push_back(std::move(ev));
    }
    shard.work_cv.notify_one();
}

bool TurnPool::idle() const {
    for (auto& shard : shards_) {
        std::lock_guard<std::mutex> lock(shard->mutex);
        if (!shard->queue.empty() || shard->busy) return false;
    }
    return true;
}

void TurnPool::drain() {
    for (auto& shard : shards_) {
        std::unique_lock<std::mutex> lock(shard->mutex);
        shard->idle_cv.wait(lock, [&] {
            return stopping_.load() || (shard->queue.empty() && !shard->busy);
        });
    }
}

void TurnPool::stop() {
    if (stopping_.exchange(true)) return;

    for (auto& shard : shards_) {
        {
            std::lock_guard<std::mutex> lock(shard->mutex);
            shard->queue.clear();
        }
        shard->work_cv.notify_all();
        shard->idle_cv.notify_all();
    }

    for (auto& thread : threads_) {
        if (thread.joinable()) thread.join();
    }
    threads_.clear();
}

void TurnPool::run(size_t index) {
    Shard& shard = *shards_[index];

    for (;;) {
        MessageReceivedEvent ev;
        {
            std::unique_lock<std::mutex> lock(shard.mutex);
            shard.work_cv.wait(lock, [&] {
                return stopping_.load() || !shard.queue.empty();
            });
            // Drop anything still queued at shutdown rather than working through
            // the backlog — the channel fails its pending callers on teardown.
            if (stopping_.load()) return;

            ev = std::move(shard.queue.front());
            shard.queue.pop_front();
            shard.busy = true;
        }

        // A turn must not take the process down. Inline on the poll loop an
        // exception reached main()'s handler and exited; on a worker thread it
        // would be std::terminate, killing every other session's turn with it.
        try {
            bus_.publish(ev);
        } catch (const std::exception& e) {
            std::cerr << "[turnpool] turn failed for session " << ev.session_id
                      << ": " << e.what() << "\n";
        } catch (...) {
            std::cerr << "[turnpool] turn failed for session " << ev.session_id
                      << ": unknown error\n";
        }

        {
            std::lock_guard<std::mutex> lock(shard.mutex);
            shard.busy = false;
        }
        shard.idle_cv.notify_all();
    }
}

} // namespace ptrclaw
