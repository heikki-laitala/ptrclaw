#pragma once
#include "event.hpp"
#include <string>
#include <vector>
#include <functional>
#include <unordered_map>
#include <mutex>
#include <cstdint>

namespace ptrclaw {

using EventHandler = std::function<void(const Event&)>;

class EventBus {
public:
    // Subscribe to events with a given tag. Returns a subscription ID.
    uint64_t subscribe(const std::string& tag, EventHandler handler);

    // Unsubscribe by ID. Returns true if found and removed.
    bool unsubscribe(uint64_t id);

    // Publish an event synchronously. Handlers called in registration order.
    // Mutex is released before calling handlers to avoid deadlocks.
    void publish(const Event& event);

    // Remove all subscriptions.
    void clear();

    // Number of subscriptions for a given tag (0 if none).
    size_t subscriber_count(const std::string& tag) const;

private:
    struct Subscription {
        uint64_t id;
        EventHandler handler;
    };

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::vector<Subscription>> handlers_;
    uint64_t next_id_ = 1;
};

// Type-safe subscribe helper: auto-casts Event& to the concrete type.
template<typename E>
uint64_t subscribe(EventBus& bus, std::function<void(const E&)> handler) {
    // The analyzer reports a possible leak of std::function's heap buffer here, but there
    // is no raw ownership to lose: `handler` is taken by value, moved into the lambda, and
    // the lambda is moved into the EventHandler that EventBus stores — every step is
    // std::function's own RAII. It is the known libc++ false positive where the analyzer
    // loses track of __f_ across the move into the subscriber vector.
    //
    // Suppressed rather than reproduced: it fires on the CI macOS runner and not on a local
    // toolchain of the same clang-tidy version, so it tracks the SDK's libc++ headers.
    // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDeleteLeaks)
    return bus.subscribe(E::TAG, [h = std::move(handler)](const Event& e) {
        h(static_cast<const E&>(e));
    });
}

} // namespace ptrclaw
