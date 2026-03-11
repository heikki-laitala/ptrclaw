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

// Deduplicate repeated log lines. Normalizes timestamps, UUIDs, hex, and
// large numbers before grouping. Returns output with counts, e.g. "[x3] msg".
std::string deduplicate_log_lines(const std::string& output);

// Rotate tee files: keep at most max_files in the directory, delete oldest.
// Truncates files larger than max_file_size bytes.
void rotate_tee_files(const std::string& tee_dir,
                      uint32_t max_files = 20,
                      uint64_t max_file_size = 1024ULL * 1024);

// Smart truncation: when output exceeds max_lines, preserve structurally
// important lines (signatures, imports, braces) and show omission markers.
std::string smart_truncate(const std::string& output, uint32_t max_lines = 200);

// Group repeated compiler/linter diagnostics by message pattern.
// E.g. 20 "unused variable" warnings become one line + "(12 more in ...files)".
std::string group_diagnostics(const std::string& output);

// Strip noise directories (node_modules, .git, target, etc.) from
// tree/find/ls -R output lines.
std::string filter_noise_dirs(const std::string& output);

} // namespace ptrclaw
