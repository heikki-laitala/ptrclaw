#pragma once
#include "channel.hpp"
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace ptrclaw {

class EventBus;

// Bridges channel display concerns with the event bus.
// Owns stream state and subscribes to message, typing, and stream events.
//
// Threading: one StreamRelay serves every session, so with TurnPool its handlers
// run on all worker threads at once and `stream_states_` needs a lock. A single
// StreamState does not: TurnPool shards by session id, so every event for one
// session is published by that session's worker and no other. The map mutex is
// therefore held only for lookup and insert — never across a channel call, which
// would let one session's slow edit_message stall another session's tokens. The
// entries are shared_ptr so a handler can keep working on a state after dropping
// the map lock.
class StreamRelay {
public:
    StreamRelay(Channel& channel, EventBus& bus);

    // Subscribe all event handlers. Call once after other handlers that
    // must run first (e.g. SessionManager) are already subscribed.
    void subscribe_events();

private:
    struct StreamState {
        std::string chat_id;
        int64_t message_id = 0;
        std::string accumulated;
        std::chrono::steady_clock::time_point last_edit;
        bool delivered = false;
    };

    // Both return nullptr when the session has no state.
    std::shared_ptr<StreamState> find_state(const std::string& session_id) const;
    std::shared_ptr<StreamState> take_state(const std::string& session_id);

    Channel& channel_;
    EventBus& bus_;

    mutable std::mutex states_mutex_;
    std::unordered_map<std::string, std::shared_ptr<StreamState>> stream_states_;
};

} // namespace ptrclaw
