#include "turn_pool.hpp"
#include "event_bus.hpp"
#include <iostream>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

namespace ptrclaw {

TurnPool::TurnPool(EventBus& bus, uint32_t workers)
    : bus_(bus), max_concurrent_(workers <= 1 ? 0 : workers) {}

TurnPool::~TurnPool() {
    stop();
}

bool TurnPool::quiet() const {
    if (in_flight_ > 0) return false;
    for (const auto& [id, session] : sessions_) {
        (void)id;
        if (!session.queue.empty()) return false;
    }
    return true;
}

uint32_t TurnPool::in_flight() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return in_flight_;
}

void TurnPool::submit(MessageReceivedEvent ev) {
    if (max_concurrent_ == 0) {
        bus_.publish(ev);
        return;
    }

    const std::string id = ev.session_id;
    {
        std::unique_lock<std::mutex> lock(mutex_);

        // Already running: queue behind it, so this session's turns stay ordered and
        // never overlap. The thread running the current turn picks this up.
        auto it = sessions_.find(id);
        if (it != sessions_.end() && it->second.running) {
            // The entry is re-looked-up on every wake rather than held across the wait:
            // the running thread may finish and erase it while this one is parked.
            space_cv_.wait(lock, [&] {
                if (stopping_.load()) return true;
                auto cur = sessions_.find(id);
                return cur == sessions_.end() || !cur->second.running ||
                       cur->second.queue.size() < kMaxQueuedPerSession;
            });
            if (stopping_.load()) return;

            auto& session = sessions_[id];
            if (session.running) {
                session.queue.push_back(std::move(ev));
                return;
            }
            // It drained while we waited, so fall through and start it here instead.
        }

        // Not running: this turn needs one of the concurrency slots.
        space_cv_.wait(lock, [&] {
            return stopping_.load() || in_flight_ < max_concurrent_;
        });
        if (stopping_.load()) return;

        sessions_[id].running = true;
        ++in_flight_;
    }

    start(id, std::move(ev));
}

void TurnPool::start(std::string session_id, MessageReceivedEvent ev) {
    // Held by shared_ptr rather than captured directly. The lambda's captures are
    // constructed before std::thread's constructor runs, so moving into them would leave
    // the originals empty by the time a creation failure is caught — the fallback would
    // then publish an empty event under an empty session id, while the real session stayed
    // marked running and wedged every turn queued behind it. Copying the shared_ptr costs
    // nothing and keeps one intact copy for whichever path runs.
    auto payload = std::make_shared<std::pair<std::string, MessageReceivedEvent>>(
        std::move(session_id), std::move(ev));
    try {
        // The payload outlives this scope through the capture, so the reference
        // run_session takes stays valid for as long as the thread runs.
        std::thread([this, payload] {
            run_session(payload->first, std::move(payload->second));
        }).detach();
    } catch (const std::system_error& e) {
        // The system will not give us a thread — a pid or thread-count limit, which a
        // container can impose well below what `workers` allows. Running the turn here
        // degrades to the inline path: slower, and correct. Dropping it or letting the
        // exception escape would lose a caller's turn over a limit that has likely
        // passed by the next request. The slot is already counted, and run_session
        // releases it either way.
        std::cerr << "[turnpool] no thread available (" << e.what()
                  << "); running this turn inline\n";
        run_session(payload->first, std::move(payload->second));
    }
}

void TurnPool::run_session(const std::string& session_id, MessageReceivedEvent first) {
    MessageReceivedEvent ev = std::move(first);

    for (;;) {
        // A turn must not take the process down. Inline on the poll loop an exception
        // reached main()'s handler and exited; on a worker thread it would be
        // std::terminate, killing every other session's turn with it.
        try {
            bus_.publish(ev);
        } catch (const std::exception& e) {
            std::cerr << "[turnpool] turn failed for session " << ev.session_id
                      << ": " << e.what() << "\n";
        } catch (...) {
            std::cerr << "[turnpool] turn failed for session " << ev.session_id
                      << ": unknown error\n";
        }

        bool more = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = sessions_.find(session_id);

            // Take this session's next turn without releasing the slot. Staying on the
            // same thread is not required for correctness — every handoff goes through
            // this mutex either way — but it avoids handing the session to a new thread
            // for no reason.
            //
            // Dropping the backlog at shutdown rather than working through it: the
            // channel fails its pending callers on teardown.
            if (!stopping_.load() && it != sessions_.end() &&
                !it->second.queue.empty()) {
                ev = std::move(it->second.queue.front());
                it->second.queue.pop_front();
                more = true;
                space_cv_.notify_all();  // room freed in this session's queue
            } else {
                if (it != sessions_.end()) {
                    it->second.running = false;
                    // Nothing left to remember about this session. Keeping the entry
                    // would leak a map slot per session id the pod has ever seen.
                    if (it->second.queue.empty()) sessions_.erase(it);
                }
                --in_flight_;

                // Notified while the mutex is still held, and that is the whole point:
                // stop() waits for in_flight_ to reach zero, and the moment it can observe
                // zero it may return and let the pool be destroyed. A notify issued after
                // releasing the lock would then be a call on a destroyed condition
                // variable — this thread is detached, so nothing else is holding the pool
                // open for it. Holding the mutex across the notify costs a wakeup that
                // immediately blocks; that is the price of the guarantee.
                space_cv_.notify_all();
                quiet_cv_.notify_all();
            }
        }

        // Past this point a finished worker touches no member of the pool, which is what
        // makes stop()'s wait an actual join for detached threads.
        if (!more) return;
    }
}

bool TurnPool::idle() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return quiet();
}

void TurnPool::drain() {
    if (max_concurrent_ == 0) return;  // inline: a turn is over before submit() returns
    std::unique_lock<std::mutex> lock(mutex_);
    quiet_cv_.wait(lock, [this] { return quiet(); });
}

void TurnPool::stop() {
    if (stopping_.exchange(true)) return;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Queued turns that never started are dropped; the ones running are waited for
        // below, because their threads are detached and would otherwise outlive the
        // pool they hold a pointer to.
        for (auto& [id, session] : sessions_) {
            (void)id;
            session.queue.clear();
        }
    }
    space_cv_.notify_all();  // release submitters parked for room or a slot

    std::unique_lock<std::mutex> lock(mutex_);
    quiet_cv_.wait(lock, [this] { return in_flight_ == 0; });
    sessions_.clear();
}

} // namespace ptrclaw
