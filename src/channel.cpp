#include "channel.hpp"

namespace ptrclaw {

std::vector<std::string> Channel::split_message(const std::string& text, size_t max_len) {
    std::vector<std::string> parts;
    if (text.empty() || max_len == 0) return parts;
    if (text.size() <= max_len) {
        parts.push_back(text);
        return parts;
    }

    size_t pos = 0;
    while (pos < text.size()) {
        size_t remaining = text.size() - pos;
        if (remaining <= max_len) {
            parts.push_back(text.substr(pos));
            break;
        }

        // Find best split point within max_len
        size_t end = pos + max_len;
        size_t split = end;

        // Prefer splitting at newline
        size_t nl = text.rfind('\n', end - 1);
        if (nl != std::string::npos && nl > pos) {
            split = nl + 1; // include the newline in current chunk
        } else {
            // Fall back to space
            size_t sp = text.rfind(' ', end - 1);
            if (sp != std::string::npos && sp > pos) {
                split = sp + 1;
            }
            // Otherwise hard cut at max_len
        }

        parts.push_back(text.substr(pos, split - pos));
        pos = split;
    }

    return parts;
}

} // namespace ptrclaw
