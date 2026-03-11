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

// Command-aware shell output filtering.
// Detects the command type (git, test runner, build) and applies smart
// filters. Falls back to generic filter_tool_output for unknown commands.
std::string filter_shell_output(const std::string& command,
                                const std::string& output,
                                const OutputFilterConfig& config = {});

// Save full shell output to disk for later retrieval.
// Returns the file path on success, empty string on failure.
// When tee_dir is empty, defaults to ~/.ptrclaw/tee/.
std::string tee_shell_output(const std::string& output,
                             const std::string& tee_dir = "");

// Extract a compact JSON schema from a JSON value.
// E.g. {"name":"John","age":42} -> {name: string, age: number}
// Returns empty string if input is not valid JSON or schema is longer.
std::string extract_json_schema(const std::string& json_str);

} // namespace ptrclaw
