#pragma once
#include "channel.hpp"
#include <string>
#include <unordered_map>
#include <chrono>
#include <cstdint>

namespace ptrclaw {

class EventBus;

// Bridges channel display concerns with the event bus.
// Owns stream state and subscribes to message, typing, and stream events.
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

    Channel& channel_;
    EventBus& bus_;
    std::unordered_map<std::string, StreamState> stream_states_;
};

} // namespace ptrclaw
