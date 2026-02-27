#include "sse.hpp"

namespace ptrclaw {

void SSEParser::feed(const std::string& chunk, const SSECallback& callback) {
    buffer_ += chunk;

    std::string current_event;
    std::string current_data;

    size_t pos = 0;
    while (pos < buffer_.size()) {
        size_t newline = buffer_.find('\n', pos);
        if (newline == std::string::npos) {
            // Incomplete line - keep remainder in buffer
            buffer_ = buffer_.substr(pos);
            return;
        }

        std::string line = buffer_.substr(pos, newline - pos);
        // Remove trailing \r if present
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        pos = newline + 1;

        if (line.empty()) {
            // Empty line = dispatch event
            if (!current_data.empty()) {
                SSEEvent event{current_event, current_data};
                if (!callback(event)) {
                    buffer_ = buffer_.substr(pos);
                    return;
                }
            }
            current_event.clear();
            current_data.clear();
        } else if (line.rfind("event: ", 0) == 0) {
            current_event = line.substr(7);
        } else if (line.rfind("data:", 0) == 0) {
            if (!current_data.empty()) {
                current_data += '\n';
            }
            // Handle both "data: payload" (with space) and "data:payload" (without)
            current_data += line.substr(line.size() > 5 && line[5] == ' ' ? 6 : 5);
        }
        // Ignore other lines (comments starting with :, etc.)
    }

    // All data processed
    buffer_.clear();
}

void SSEParser::reset() {
    buffer_.clear();
}

} // namespace ptrclaw
