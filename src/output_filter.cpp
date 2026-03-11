#include "output_filter.hpp"
#include "util.hpp"
#include <filesystem>
#include <nlohmann/json.hpp>
#include <sstream>
#include <vector>

namespace ptrclaw {

std::string strip_ansi_codes(const std::string& input) {
    std::string result;
    result.reserve(input.size());

    size_t i = 0;
    while (i < input.size()) {
        if (input[i] == '\033' && i + 1 < input.size() && input[i + 1] == '[') {
            // Skip ESC [ ... <final byte>
            i += 2;
            while (i < input.size() && input[i] < 0x40) {
                i++;
            }
            if (i < input.size()) i++; // skip final byte
        } else {
            result += input[i];
            i++;
        }
    }
    return result;
}

std::string filter_tool_output(const std::string& output,
                               const OutputFilterConfig& config) {
    if (output.empty()) return output;

    std::string cleaned = config.strip_ansi ? strip_ansi_codes(output) : output;

    // Split into lines, apply per-line and total limits
    std::ostringstream result;
    uint32_t line_count = 0;
    uint32_t total_chars = 0;
    bool prev_blank = false;
    bool truncated = false;

    size_t pos = 0;
    while (pos < cleaned.size()) {
        // Find end of line
        size_t eol = cleaned.find('\n', pos);
        size_t line_end = (eol != std::string::npos) ? eol : cleaned.size();
        std::string line = cleaned.substr(pos, line_end - pos);
        pos = (eol != std::string::npos) ? eol + 1 : cleaned.size();

        // Collapse consecutive blank lines
        bool is_blank = line.find_first_not_of(" \t\r") == std::string::npos;
        if (config.collapse_blank_lines && is_blank) {
            if (prev_blank) continue;
            prev_blank = true;
        } else {
            prev_blank = false;
        }

        // Truncate long lines
        if (config.max_line_length > 0 && line.size() > config.max_line_length) {
            line = line.substr(0, config.max_line_length) + "...";
        }

        // Check limits
        if (config.max_lines > 0 && line_count >= config.max_lines) {
            truncated = true;
            break;
        }
        if (config.max_total_chars > 0 &&
            total_chars + line.size() + 1 > config.max_total_chars) {
            truncated = true;
            break;
        }

        if (line_count > 0) result << '\n';
        result << line;
        total_chars += line.size() + 1;
        line_count++;
    }

    if (truncated) {
        // Count remaining lines
        uint32_t remaining = 0;
        while (pos < cleaned.size()) {
            if (cleaned[pos] == '\n') remaining++;
            pos++;
        }
        if (remaining > 0 || line_count < cleaned.size()) {
            result << "\n[..." << remaining << " more lines truncated]";
        }
    }

    return result.str();
}

// ── Helper: split string into lines ─────────────────────────────

static std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) {
            lines.push_back(text.substr(pos));
            break;
        }
        lines.push_back(text.substr(pos, eol - pos));
        pos = eol + 1;
    }
    return lines;
}

static std::string join_lines(const std::vector<std::string>& lines) {
    std::string result;
    for (size_t i = 0; i < lines.size(); i++) {
        if (i > 0) result += '\n';
        result += lines[i];
    }
    return result;
}

// ── Command classifier ──────────────────────────────────────────

enum class ShellCommandType { GitDiff, GitStatus, GitLog, TestRunner, BuildLog, Other };

static bool starts_with(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

static bool contains(const std::string& s, const std::string& needle) {
    return s.find(needle) != std::string::npos;
}

static ShellCommandType classify_command(const std::string& command) {
    // Trim leading whitespace / env vars for matching
    std::string cmd = command;
    size_t start = cmd.find_first_not_of(" \t");
    if (start != std::string::npos) cmd = cmd.substr(start);

    if (starts_with(cmd, "git diff") || contains(cmd, "| git diff")) return ShellCommandType::GitDiff;
    if (starts_with(cmd, "git status")) return ShellCommandType::GitStatus;
    if (starts_with(cmd, "git log") || starts_with(cmd, "git shortlog")) return ShellCommandType::GitLog;

    // Test runners
    if (contains(cmd, "pytest") || contains(cmd, "cargo test") ||
        contains(cmd, "npm test") || contains(cmd, "npx jest") ||
        contains(cmd, "go test") || contains(cmd, "make test") ||
        contains(cmd, "ctest")) {
        return ShellCommandType::TestRunner;
    }

    // Build tools
    if (starts_with(cmd, "make") || starts_with(cmd, "cmake --build") ||
        starts_with(cmd, "cargo build") || starts_with(cmd, "cargo clippy") ||
        starts_with(cmd, "npm run build") || starts_with(cmd, "go build") ||
        starts_with(cmd, "ninja") || starts_with(cmd, "meson compile")) {
        return ShellCommandType::BuildLog;
    }

    return ShellCommandType::Other;
}

// ── Per-command filters ─────────────────────────────────────────

static std::string filter_git_diff(const std::string& output) {
    auto lines = split_lines(output);
    std::vector<std::string> kept;

    for (const auto& line : lines) {
        if (line.empty()) continue;

        // Always keep: diff headers, file markers, hunk headers, changes, stats
        if (starts_with(line, "diff --git") ||
            starts_with(line, "---") ||
            starts_with(line, "+++") ||
            starts_with(line, "@@") ||
            starts_with(line, "+") ||
            starts_with(line, "-") ||
            starts_with(line, "index ") ||
            starts_with(line, "new file") ||
            starts_with(line, "deleted file") ||
            starts_with(line, "rename") ||
            starts_with(line, "similarity") ||
            starts_with(line, "Binary files")) {
            kept.push_back(line);
            continue;
        }

        // Keep stat summary lines (e.g. "3 files changed, 10 insertions(+)")
        if (contains(line, "file") && contains(line, "changed")) {
            kept.push_back(line);
            continue;
        }

        // Drop context lines (lines starting with space in unified diff)
    }

    return join_lines(kept);
}

static std::string filter_git_status(const std::string& output) {
    auto lines = split_lines(output);
    std::vector<std::string> kept;

    for (const auto& line : lines) {
        // Drop hint lines from git status
        if (contains(line, "(use \"git")) continue;

        kept.push_back(line);
    }

    return join_lines(kept);
}

static std::string filter_test_output(const std::string& output) {
    auto lines = split_lines(output);
    std::vector<std::string> kept;

    // Two passes: collect failure lines and summary, with context
    std::vector<bool> keep(lines.size(), false);

    for (size_t i = 0; i < lines.size(); i++) {
        const auto& line = lines[i];

        // Failure indicators
        if (contains(line, "FAIL") || contains(line, "FAILED") ||
            contains(line, "ERROR") || contains(line, "error[") ||
            contains(line, "ERRORS") || contains(line, "panicked") ||
            contains(line, "AssertionError") || contains(line, "assert")) {
            // Keep this line + 2 lines of context after (for stack traces)
            size_t from = (i >= 1) ? i - 1 : 0;
            for (size_t j = from; j <= std::min(i + 2, lines.size() - 1); j++) {
                keep[j] = true;
            }
        }

        // Summary lines (typically near the end)
        if (contains(line, "passed") || contains(line, "failed") ||
            contains(line, "test result") || contains(line, "Tests:") ||
            contains(line, "tests passed") || contains(line, "tests failed") ||
            contains(line, "Test Suites:") || contains(line, "ok.") ||
            contains(line, "FAILED.") || contains(line, "failures")) {
            keep[i] = true;
        }
    }

    bool in_gap = false;
    for (size_t i = 0; i < lines.size(); i++) {
        if (keep[i]) {
            if (in_gap) {
                kept.emplace_back("[...]");
                in_gap = false;
            }
            kept.push_back(lines[i]);
        } else {
            in_gap = true;
        }
    }

    // If nothing was kept (all passed?), return a compact summary
    if (kept.empty()) {
        // Find and return just the last few lines (likely summary)
        size_t start = lines.size() > 5 ? lines.size() - 5 : 0;
        for (size_t i = start; i < lines.size(); i++) {
            kept.push_back(lines[i]);
        }
    }

    return join_lines(kept);
}

static std::string filter_build_log(const std::string& output) {
    auto lines = split_lines(output);
    std::vector<std::string> kept;

    for (size_t i = 0; i < lines.size(); i++) {
        const auto& line = lines[i];

        // Keep error and warning lines + 1 line before for file context
        if (contains(line, "error") || contains(line, "Error") ||
            contains(line, "warning:") || contains(line, "Warning:") ||
            contains(line, "undefined reference") ||
            contains(line, "cannot find") || contains(line, "fatal")) {
            if (i > 0 && (kept.empty() || kept.back() != lines[i - 1])) {
                kept.push_back(lines[i - 1]);
            }
            kept.push_back(line);
            continue;
        }

        // Drop progress indicators
        if (contains(line, "Compiling ") || contains(line, "Linking ") ||
            contains(line, "Building ") || contains(line, "Downloading ") ||
            contains(line, "Generating ") || contains(line, "Installing ")) {
            continue;
        }
        // Drop percentage progress (e.g. "[  5%]", "[12/34]")
        if (line.size() > 1 && line[0] == '[' &&
            (std::isdigit(static_cast<unsigned char>(line[1])) || line[1] == ' ')) {
            continue;
        }

        // Keep summary lines (typically at end)
        if (contains(line, "Build") && (contains(line, "succeeded") || contains(line, "failed"))) {
            kept.push_back(line);
            continue;
        }
        if (contains(line, "error generated") || contains(line, "warnings generated")) {
            kept.push_back(line);
            continue;
        }

        // Keep ninja/make summary
        if (starts_with(line, "ninja:") || starts_with(line, "make:") || starts_with(line, "make[")) {
            kept.push_back(line);
        }
    }

    // Always include last few lines (often summary)
    if (lines.size() > 3) {
        size_t tail_start = lines.size() - 3;
        for (size_t i = tail_start; i < lines.size(); i++) {
            // Avoid duplicating lines already kept
            bool already_kept = false;
            for (const auto& k : kept) {
                if (k == lines[i]) { already_kept = true; break; }
            }
            if (!already_kept && !lines[i].empty()) {
                kept.push_back(lines[i]);
            }
        }
    }

    return join_lines(kept);
}

// ── Public API ──────────────────────────────────────────────────

std::string filter_shell_output(const std::string& command,
                                const std::string& output,
                                const OutputFilterConfig& config) {
    if (output.empty()) return output;

    // Strip ANSI first
    std::string cleaned = config.strip_ansi ? strip_ansi_codes(output) : output;

    // Apply command-specific filter
    std::string filtered;
    switch (classify_command(command)) {
        case ShellCommandType::GitDiff:    filtered = filter_git_diff(cleaned);    break;
        case ShellCommandType::GitStatus:  filtered = filter_git_status(cleaned);  break;
        case ShellCommandType::GitLog:     filtered = cleaned; break;
        case ShellCommandType::TestRunner: filtered = filter_test_output(cleaned); break;
        case ShellCommandType::BuildLog:   filtered = filter_build_log(cleaned);   break;
        case ShellCommandType::Other: {
            // Try JSON schema extraction for API/curl responses
            std::string schema = extract_json_schema(cleaned);
            filtered = schema.empty() ? cleaned : schema;
            break;
        }
    }

    // Apply generic limits as final pass
    OutputFilterConfig final_config = config;
    final_config.strip_ansi = false; // already stripped
    return filter_tool_output(filtered, final_config);
}

// ── JSON schema extraction ───────────────────────────────────────

static std::string json_type_name(const nlohmann::json& val) {
    if (val.is_null())    return "null";
    if (val.is_boolean()) return "bool";
    if (val.is_number_integer()) return "int";
    if (val.is_number())  return "number";
    if (val.is_string())  return "string";
    if (val.is_array())   return "array";
    if (val.is_object())  return "object";
    return "unknown";
}

static std::string schema_for_value(const nlohmann::json& val, int depth);

static std::string schema_for_object(const nlohmann::json& obj, int depth) {
    if (obj.empty()) return "{}";
    std::string result = "{";
    bool first = true;
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        if (!first) result += ", ";
        first = false;
        result += it.key() + ": " + schema_for_value(it.value(), depth + 1);
    }
    result += "}";
    return result;
}

static std::string schema_for_array(const nlohmann::json& arr, int depth) {
    if (arr.empty()) return "[]";
    // Use the first element as representative
    return "[" + schema_for_value(arr[0], depth + 1) + "]";
}

static std::string schema_for_value(const nlohmann::json& val, int depth) {
    if (depth > 4) return "..."; // prevent deep recursion
    if (val.is_object()) return schema_for_object(val, depth);
    if (val.is_array())  return schema_for_array(val, depth);
    return json_type_name(val);
}

std::string extract_json_schema(const std::string& json_str) {
    // Quick check: must start with { or [
    auto start = json_str.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    if (json_str[start] != '{' && json_str[start] != '[') return "";

    try {
        auto j = nlohmann::json::parse(json_str);
        std::string schema = schema_for_value(j, 0);

        // Only use schema if it's meaningfully shorter
        if (schema.size() < json_str.size() * 3 / 4) {
            return schema;
        }
    } catch (...) { // NOLINT(bugprone-empty-catch)
        // Not valid JSON
    }
    return "";
}

std::string tee_shell_output(const std::string& output,
                             const std::string& tee_dir) {
    if (output.empty()) return "";

    std::string dir = tee_dir.empty() ? expand_home("~/.ptrclaw/tee") : tee_dir;

    try {
        std::filesystem::create_directories(dir);
    } catch (...) {
        return "";
    }

    std::string filename = std::to_string(epoch_seconds()) + "_" + generate_id() + ".log";
    std::string path = dir + "/" + filename;

    if (atomic_write_file(path, output)) {
        return path;
    }
    return "";
}

} // namespace ptrclaw
