#pragma once
#include <string>
#include <cstdint>

namespace ptrclaw {

struct OutputFilterConfig {
    uint32_t max_lines = 200;          // cap output at N lines
    uint32_t max_line_length = 500;    // truncate individual lines
    uint32_t max_total_chars = 20000;  // hard character limit
    bool strip_ansi = true;            // remove ANSI escape codes
    bool collapse_blank_lines = true;  // consecutive blank lines -> one
};

// Filter tool output to reduce token consumption.
// Returns the (possibly truncated) output string.
std::string filter_tool_output(const std::string& output,
                               const OutputFilterConfig& config = {});

// Strip ANSI escape sequences from a string.
std::string strip_ansi_codes(const std::string& input);

} // namespace ptrclaw
