#pragma once
#include <string>
#include <functional>

namespace ptrclaw {

struct SSEEvent {
    std::string event; // event type (e.g., "message_start", "content_block_delta")
    std::string data;  // raw JSON data
};

// Callback receives each parsed SSE event. Return false to stop parsing.
using SSECallback = std::function<bool(const SSEEvent& event)>;

// Parse a stream of SSE data, calling the callback for each complete event
class SSEParser {
public:
    // Feed raw data chunk, triggers callback for complete events
    void feed(const std::string& chunk, const SSECallback& callback);

    // Reset parser state
    void reset();

private:
    std::string buffer_;
};

} // namespace ptrclaw
