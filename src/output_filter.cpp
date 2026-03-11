#include "output_filter.hpp"
#include "util.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <regex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
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

enum class ShellCommandType {
    GitDiff, GitStatus, GitLog, TestRunner, BuildLog, DirListing,
    Linter, SearchResult, HttpResponse, ContainerOps, PackageManager, Other
};

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

    // Build tools (make without test/check suffixes — those are caught above)
    if ((starts_with(cmd, "make") && !contains(cmd, "test") && !contains(cmd, "check")) ||
        starts_with(cmd, "cmake --build") ||
        starts_with(cmd, "cargo build") || starts_with(cmd, "cargo clippy") ||
        starts_with(cmd, "npm run build") || starts_with(cmd, "go build") ||
        starts_with(cmd, "ninja") || starts_with(cmd, "meson compile")) {
        return ShellCommandType::BuildLog;
    }

    // Linters (before build tools — some overlap with cargo clippy)
    if (starts_with(cmd, "eslint") || contains(cmd, "npx eslint") ||
        starts_with(cmd, "tsc") || contains(cmd, "npx tsc") ||
        starts_with(cmd, "ruff ") || starts_with(cmd, "pylint") ||
        starts_with(cmd, "flake8") || starts_with(cmd, "mypy") ||
        starts_with(cmd, "golangci-lint") ||
        starts_with(cmd, "biome ") || contains(cmd, "npx biome")) {
        return ShellCommandType::Linter;
    }

    // Directory listing tools
    if (starts_with(cmd, "tree") || starts_with(cmd, "find ") ||
        starts_with(cmd, "ls -") || starts_with(cmd, "fd ")) {
        return ShellCommandType::DirListing;
    }

    // Search/grep tools
    if (starts_with(cmd, "grep ") || starts_with(cmd, "rg ") ||
        starts_with(cmd, "ag ") || starts_with(cmd, "ack ") ||
        starts_with(cmd, "git grep")) {
        return ShellCommandType::SearchResult;
    }

    // HTTP clients
    if (starts_with(cmd, "curl ") || starts_with(cmd, "wget ") ||
        starts_with(cmd, "http ") || starts_with(cmd, "https ")) {
        return ShellCommandType::HttpResponse;
    }

    // Container/orchestration
    if (starts_with(cmd, "docker ") || starts_with(cmd, "podman ") ||
        starts_with(cmd, "kubectl ") || starts_with(cmd, "k ")) {
        return ShellCommandType::ContainerOps;
    }

    // Package managers
    if ((starts_with(cmd, "npm ") && (contains(cmd, "list") || contains(cmd, "ls") || contains(cmd, "outdated"))) ||
        (starts_with(cmd, "pnpm ") && (contains(cmd, "list") || contains(cmd, "ls") || contains(cmd, "outdated"))) ||
        (starts_with(cmd, "yarn ") && (contains(cmd, "list") || contains(cmd, "why"))) ||
        (starts_with(cmd, "pip ") && (contains(cmd, "list") || contains(cmd, "freeze"))) ||
        starts_with(cmd, "cargo tree") ||
        (starts_with(cmd, "gem ") && contains(cmd, "list")) ||
        (starts_with(cmd, "brew ") && contains(cmd, "list"))) {
        return ShellCommandType::PackageManager;
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

    // Always include last few lines (often summary).
    // Use a set to avoid O(n²) duplicate checking.
    if (lines.size() > 3) {
        std::unordered_set<std::string> kept_set(kept.begin(), kept.end());
        size_t tail_start = lines.size() - 3;
        for (size_t i = tail_start; i < lines.size(); i++) {
            if (!lines[i].empty() && kept_set.find(lines[i]) == kept_set.end()) {
                kept.push_back(lines[i]);
            }
        }
    }

    return join_lines(kept);
}

// ── Linter output filter ────────────────────────────────────────
// Groups linter diagnostics by rule/message pattern. Keeps first occurrence
// of each unique message, collapses duplicates with file list.

static std::string filter_linter_output(const std::string& output) {
    auto lines = split_lines(output);
    if (lines.size() < 5) return output;

    // Linter output is already diagnostic-heavy; apply grouping directly
    std::string grouped = group_diagnostics(output);

    // Additionally strip common linter noise
    auto glines = split_lines(grouped);
    std::vector<std::string> kept;
    for (const auto& line : glines) {
        // ESLint/biome: skip per-file summary lines with 0 issues
        // Match "✓ 0 problems" style lines but not "5 problems (0 errors, 5 warnings)"
        if (starts_with(line, "✓") && contains(line, "0 problems")) continue;
        // ruff/pylint: skip "Found N errors" (redundant with individual lines)
        // tsc: skip blank lines between errors
        if (line.empty() && !kept.empty() && kept.back().empty()) continue;
        kept.push_back(line);
    }

    // Keep summary line at end if present
    return join_lines(kept);
}

// ── Search result filter ────────────────────────────────────────
// Groups grep/rg/ag results by file, caps matches per file.

static std::string filter_search_results(const std::string& output) {
    auto lines = split_lines(output);
    if (lines.size() < 10) return output;

    // Group matches by file prefix (file:line:content or file-line-content)
    std::unordered_map<std::string, std::vector<size_t>> file_matches;
    std::vector<std::string> non_match_lines;
    std::vector<std::string> file_order;
    constexpr uint32_t max_matches_per_file = 5;

    for (size_t i = 0; i < lines.size(); i++) {
        const auto& line = lines[i];
        // Detect file:line: pattern (grep -n style)
        auto colon1 = line.find(':');
        if (colon1 != std::string::npos && colon1 > 0 && colon1 < line.size() - 1) {
            auto colon2 = line.find(':', colon1 + 1);
            if (colon2 != std::string::npos) {
                std::string file = line.substr(0, colon1);
                // Verify it looks like a file path (has / or .)
                if (file.find('/') != std::string::npos || file.find('.') != std::string::npos) {
                    if (file_matches.find(file) == file_matches.end()) {
                        file_order.push_back(file);
                    }
                    file_matches[file].push_back(i);
                    continue;
                }
            }
        }
        // Lines without file prefix (headers, separators, etc.)
        non_match_lines.push_back(line);
    }

    // If no file grouping detected, return original
    if (file_matches.empty()) return output;

    std::vector<std::string> result;
    for (const auto& nl : non_match_lines) {
        result.push_back(nl);
    }

    for (const auto& file : file_order) {
        const auto& indices = file_matches[file];
        uint32_t shown = 0;
        for (size_t idx : indices) {
            if (shown < max_matches_per_file) {
                result.push_back(lines[idx]);
                shown++;
            }
        }
        if (indices.size() > max_matches_per_file) {
            result.push_back("  [..." + std::to_string(indices.size() - max_matches_per_file) +
                             " more matches in " + file + "]");
        }
    }

    uint32_t total = 0;
    for (const auto& [f, idxs] : file_matches) total += static_cast<uint32_t>(idxs.size());
    result.push_back("[" + std::to_string(total) + " matches in " +
                     std::to_string(file_matches.size()) + " files]");

    return join_lines(result);
}

// ── HTTP response filter ────────────────────────────────────────
// Strips verbose headers from curl/wget output, keeps status + body.

static std::string filter_http_response(const std::string& output) {
    auto lines = split_lines(output);
    if (lines.empty()) return output;

    std::vector<std::string> kept;
    bool in_headers = false;
    bool past_headers = false;
    uint32_t header_count = 0;

    for (const auto& line : lines) {
        // curl -v: lines starting with < are response headers, > are request headers
        if (starts_with(line, "> ") || starts_with(line, "* ") ||
            line == ">" || line == "*") {
            // Request headers and connection info — skip
            continue;
        }

        if (starts_with(line, "< ") || line == "<") {
            if (line == "<") {
                // curl -v separator line between headers and body
                in_headers = false;
                past_headers = true;
                if (header_count > 2) {
                    kept.push_back("[" + std::to_string(header_count - 2) + " headers stripped]");
                }
                continue;
            }
            // Response header
            std::string hdr = line.substr(2);
            // Keep status line and content-type
            if (starts_with(hdr, "HTTP/") ||
                contains(hdr, "content-type") || contains(hdr, "Content-Type")) {
                kept.push_back(hdr);
            }
            in_headers = true;
            header_count++;
            continue;
        }

        // Detect raw HTTP response headers (curl -i or wget -S)
        if (!past_headers && (starts_with(line, "HTTP/") ||
            (contains(line, ":") && !contains(line, "{") && line.size() < 200 &&
             line.find(':') < 40))) {
            if (starts_with(line, "HTTP/") ||
                contains(line, "content-type") || contains(line, "Content-Type")) {
                kept.push_back(line);
            }
            in_headers = true;
            header_count++;
            continue;
        }

        // Empty line after headers = body starts
        if (in_headers && line.empty()) {
            in_headers = false;
            past_headers = true;
            if (header_count > 2) {
                kept.push_back("[" + std::to_string(header_count - 2) + " headers stripped]");
            }
            continue;
        }

        past_headers = true;
        kept.push_back(line);
    }

    std::string result = join_lines(kept);

    // Try JSON schema extraction on the body
    std::string schema = extract_json_schema(result);
    if (!schema.empty()) return schema;

    return result;
}

// ── Container output filter ─────────────────────────────────────
// Compacts docker/kubectl output: truncates wide table columns,
// strips verbose YAML/JSON metadata.

static std::string filter_container_output(const std::string& output) {
    auto lines = split_lines(output);
    if (lines.size() < 3) return output;

    std::vector<std::string> kept;
    bool in_yaml_metadata = false;
    uint32_t metadata_skipped = 0;

    for (const auto& line : lines) {
        // kubectl: skip verbose metadata blocks in YAML output
        if (line == "metadata:" || starts_with(line, "  metadata:")) {
            in_yaml_metadata = true;
            kept.push_back(line);
            continue;
        }
        if (in_yaml_metadata) {
            // Still in metadata if indented more than "metadata:" level
            if (starts_with(line, "    ") || starts_with(line, "\t\t")) {
                // Keep name/namespace, skip the rest
                if (contains(line, "name:") || contains(line, "namespace:")) {
                    kept.push_back(line);
                } else {
                    metadata_skipped++;
                }
                continue;
            }
            in_yaml_metadata = false;
            if (metadata_skipped > 0) {
                kept.push_back("    [" + std::to_string(metadata_skipped) + " metadata fields stripped]");
                metadata_skipped = 0;
            }
        }

        // docker ps/images: truncate wide COMMAND columns and IMAGE IDs
        // Keep line but cap its length for table output
        if (line.size() > 200) {
            kept.push_back(line.substr(0, 200) + "...");
        } else {
            kept.push_back(line);
        }
    }

    if (metadata_skipped > 0) {
        kept.push_back("    [" + std::to_string(metadata_skipped) + " metadata fields stripped]");
    }

    return join_lines(kept);
}

// ── Package manager filter ──────────────────────────────────────
// Collapses dependency trees, strips progress bars.

static std::string filter_package_output(const std::string& output) {
    auto lines = split_lines(output);
    if (lines.size() < 10) return output;

    std::vector<std::string> kept;
    uint32_t deep_deps = 0;
    int max_depth = 2; // Keep top 2 levels of dep tree

    for (const auto& line : lines) {
        // Determine tree depth by counting "│   " or "    " prefix groups.
        // Tree output uses 4-char columns per level: "│   ", "├── ", "└── ", "    "
        // Each level contributes ~4 visible columns of prefix.
        int depth = 0;
        size_t pos = 0;
        while (pos < line.size()) {
            auto ch = static_cast<unsigned char>(line[pos]);
            if (ch == ' ') { pos++; continue; }
            // ASCII tree chars: |, +, -, `, backslash
            if (ch == '|' || ch == '+' || ch == '-' || ch == '`' || ch == '\\') {
                pos++; continue;
            }
            // UTF-8 box-drawing (U+2500..U+257F encoded as 0xE2 0x94/0x95 xx)
            if (ch == 0xE2 && pos + 2 < line.size()) {
                auto next = static_cast<unsigned char>(line[pos + 1]);
                if (next == 0x94 || next == 0x95) {
                    pos += 3; continue; // skip box-drawing char
                }
            }
            break; // reached actual content
        }
        // Depth = how many 4-char column groups precede the content
        depth = static_cast<int>(pos / 4);

        // npm/pnpm indentation: pure spaces, 2 per level
        if (pos == 0) {
            size_t leading_spaces = line.find_first_not_of(' ');
            if (leading_spaces != std::string::npos && leading_spaces > 0) {
                depth = static_cast<int>(leading_spaces / 2);
            }
        }

        if (depth > max_depth) {
            deep_deps++;
            continue;
        }

        kept.push_back(line);
    }

    if (deep_deps > 0) {
        kept.push_back("[" + std::to_string(deep_deps) + " transitive dependencies hidden]");
    }

    return join_lines(kept);
}

// ── Short-circuit rules ─────────────────────────────────────────
// If output matches a known success pattern (and no error indicator),
// return a one-line summary instead of processing the full output.

// Check if output matches a known success pattern without any blocker keywords.
// Returns the replacement message, or empty string if no match.
static std::string try_build_short_circuit(const std::string& output) {
    // Blockers: if any of these appear, don't short-circuit
    if (contains(output, "error") || contains(output, "warning") ||
        contains(output, "FAILED")) {
        return "";
    }

    if (contains(output, "Finished")) return "Build succeeded";         // cargo
    if (contains(output, "BUILD SUCCESSFUL")) return "Build succeeded"; // gradle

    return "";
}

// ── Public API ──────────────────────────────────────────────────

std::string filter_shell_output(const std::string& command,
                                const std::string& output,
                                const OutputFilterConfig& config) {
    if (output.empty()) return output;

    // Strip ANSI first
    std::string cleaned = config.strip_ansi ? strip_ansi_codes(output) : output;

    auto cmd_type = classify_command(command);

    // Try short-circuit before full filtering
    if (cmd_type == ShellCommandType::BuildLog) {
        std::string sc = try_build_short_circuit(cleaned);
        if (!sc.empty()) return sc;
    }

    // Apply command-specific filter
    std::string filtered;
    switch (cmd_type) {
        case ShellCommandType::GitDiff:    filtered = filter_git_diff(cleaned);    break;
        case ShellCommandType::GitStatus:  filtered = filter_git_status(cleaned);  break;
        case ShellCommandType::GitLog:     filtered = cleaned; break;
        case ShellCommandType::TestRunner: filtered = filter_test_output(cleaned); break;
        case ShellCommandType::BuildLog:
            filtered = filter_build_log(cleaned);
            filtered = group_diagnostics(filtered);
            break;
        case ShellCommandType::DirListing:    filtered = filter_noise_dirs(cleaned);      break;
        case ShellCommandType::Linter:        filtered = filter_linter_output(cleaned);   break;
        case ShellCommandType::SearchResult:  filtered = filter_search_results(cleaned);  break;
        case ShellCommandType::HttpResponse:  filtered = filter_http_response(cleaned);   break;
        case ShellCommandType::ContainerOps:  filtered = filter_container_output(cleaned); break;
        case ShellCommandType::PackageManager: filtered = filter_package_output(cleaned);  break;
        case ShellCommandType::Other: {
            // Try JSON schema extraction for API/curl responses
            std::string schema = extract_json_schema(cleaned);
            filtered = schema.empty() ? cleaned : schema;
            break;
        }
    }

    // Apply generic limits as final pass.
    // Smart truncation replaces the naive line-count cut, so disable max_lines
    // in filter_tool_output to avoid double truncation.
    if (config.max_lines > 0) {
        filtered = smart_truncate(filtered, config.max_lines);
    }
    OutputFilterConfig final_config = config;
    final_config.strip_ansi = false;  // already stripped
    final_config.max_lines = 0;       // already handled by smart_truncate
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
        // Rotate lazily after successful write
        rotate_tee_files(dir);
        return path;
    }
    return "";
}

// ── Tee file rotation ───────────────────────────────────────────

void rotate_tee_files(const std::string& tee_dir,
                      uint32_t max_files, uint64_t max_file_size) {
    namespace fs = std::filesystem;
    if (tee_dir.empty() || !fs::exists(tee_dir)) return;

    // Collect .log files sorted by modification time (oldest first)
    std::vector<fs::directory_entry> logs;
    try {
        for (const auto& entry : fs::directory_iterator(tee_dir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".log") {
                logs.push_back(entry);
            }
        }
    } catch (...) {
        return;
    }

    // Early exit if within limits
    if (logs.size() <= max_files) {
        bool any_oversized = false;
        for (const auto& entry : logs) {
            try {
                if (entry.file_size() > max_file_size) { any_oversized = true; break; }
            } catch (...) {}
        }
        if (!any_oversized) return;
    }

    std::sort(logs.begin(), logs.end(),
              [](const fs::directory_entry& a, const fs::directory_entry& b) {
                  return fs::last_write_time(a) < fs::last_write_time(b);
              });

    // Delete oldest files if over limit
    while (logs.size() > max_files) {
        try { fs::remove(logs.front().path()); } catch (...) {} // NOLINT(bugprone-empty-catch)
        logs.erase(logs.begin());
    }

    // Truncate oversized files
    for (const auto& entry : logs) {
        try {
            if (entry.file_size() > max_file_size) {
                // Rewrite with truncation marker
                std::ifstream in(entry.path(), std::ios::binary);
                std::string head(max_file_size, '\0');
                in.read(head.data(), static_cast<std::streamsize>(max_file_size));
                head.resize(static_cast<size_t>(in.gcount()));
                in.close();

                std::ofstream out(entry.path(), std::ios::binary | std::ios::trunc);
                out << head << "\n[...truncated at " << max_file_size << " bytes]";
            }
        } catch (...) {} // NOLINT(bugprone-empty-catch)
    }
}

// ── Log deduplication ───────────────────────────────────────────

// Normalize a log line by stripping timestamps, UUIDs, hex, and large numbers
static std::string normalize_log_line(const std::string& line) {
    // These are compiled once (static) to avoid repeated construction
    static const std::regex timestamp_re(
        R"(\d{4}[-/]\d{2}[-/]\d{2}[T ]\d{2}:\d{2}:\d{2}[.,]?\d*\s*)");
    static const std::regex uuid_re(
        R"([0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12})");
    static const std::regex hex_re(R"(0x[0-9a-fA-F]+)");
    static const std::regex num_re(R"(\b\d{4,}\b)");
    static const std::regex path_re(R"(/[\w./\-]+)");

    std::string norm = line;
    norm = std::regex_replace(norm, timestamp_re, "");
    norm = std::regex_replace(norm, uuid_re, "<UUID>");
    norm = std::regex_replace(norm, hex_re, "<HEX>");
    norm = std::regex_replace(norm, num_re, "<N>");
    norm = std::regex_replace(norm, path_re, "<PATH>");
    return norm;
}

std::string deduplicate_log_lines(const std::string& output) {
    if (output.empty()) return output;

    auto lines = split_lines(output);
    if (lines.size() < 10) return output;  // not worth deduplicating short output

    // Group consecutive identical (normalized) lines
    struct Group {
        std::string original;  // first occurrence (un-normalized)
        std::string normalized;
        uint32_t count = 1;
    };

    std::vector<Group> groups;
    for (const auto& line : lines) {
        std::string norm = normalize_log_line(line);
        if (!groups.empty() && groups.back().normalized == norm) {
            groups.back().count++;
        } else {
            groups.push_back({line, norm, 1});
        }
    }

    // If no deduplication happened, return original
    if (groups.size() == lines.size()) return output;

    std::vector<std::string> result;
    result.reserve(groups.size());
    for (const auto& g : groups) {
        if (g.count > 1) {
            result.push_back("[x" + std::to_string(g.count) + "] " + g.original);
        } else {
            result.push_back(g.original);
        }
    }

    return join_lines(result);
}

// ── Smart truncation ────────────────────────────────────────────

static bool is_structural_line(const std::string& line) {
    std::string trimmed = line;
    auto start = trimmed.find_first_not_of(" \t");
    if (start != std::string::npos) trimmed = trimmed.substr(start);

    // Function/method signatures
    if (starts_with(trimmed, "def ") || starts_with(trimmed, "fn ") ||
        starts_with(trimmed, "func ") || starts_with(trimmed, "function ") ||
        starts_with(trimmed, "class ") || starts_with(trimmed, "struct ") ||
        starts_with(trimmed, "impl ") || starts_with(trimmed, "interface ") ||
        starts_with(trimmed, "enum ") || starts_with(trimmed, "trait ")) {
        return true;
    }

    // Imports / includes
    if (starts_with(trimmed, "import ") || starts_with(trimmed, "from ") ||
        starts_with(trimmed, "#include") || starts_with(trimmed, "use ") ||
        starts_with(trimmed, "require(") || starts_with(trimmed, "export ")) {
        return true;
    }

    // Scope boundaries
    if (trimmed == "}" || trimmed == "};" || trimmed == "{") {
        return true;
    }

    // Error/warning lines (always keep) — match diagnostic patterns, not substrings
    if (contains(trimmed, "error:") || contains(trimmed, "Error:") ||
        contains(trimmed, "warning:") || contains(trimmed, "FAIL")) {
        return true;
    }

    return false;
}

std::string smart_truncate(const std::string& output, uint32_t max_lines) {
    auto lines = split_lines(output);
    if (lines.size() <= max_lines) return output;

    // Reserve slots: first 20% for head, last 20% for tail, middle for important lines
    uint32_t head_count = max_lines / 5;
    uint32_t tail_count = max_lines / 5;
    uint32_t middle_budget = max_lines - head_count - tail_count - 1; // -1 for marker

    std::vector<std::string> result;
    result.reserve(max_lines + 2);

    // Head section
    for (uint32_t i = 0; i < head_count && i < lines.size(); i++) {
        result.push_back(lines[i]);
    }

    // Middle: scan for structural lines
    size_t middle_start = head_count;
    size_t middle_end = lines.size() - tail_count;
    uint32_t structural_kept = 0;
    size_t last_kept_idx = head_count;

    for (size_t i = middle_start; i < middle_end && structural_kept < middle_budget; i++) {
        if (is_structural_line(lines[i])) {
            if (i > last_kept_idx + 1) {
                auto skipped = static_cast<uint32_t>(i - last_kept_idx - 1);
                result.push_back("[..." + std::to_string(skipped) + " lines omitted]");
            }
            result.push_back(lines[i]);
            structural_kept++;
            last_kept_idx = i;
        }
    }

    // Omission marker for remaining middle
    if (middle_end > last_kept_idx + 1) {
        auto skipped = static_cast<uint32_t>(middle_end - last_kept_idx - 1);
        result.push_back("[..." + std::to_string(skipped) + " lines omitted]");
    }

    // Tail section
    for (size_t i = middle_end; i < lines.size(); i++) {
        result.push_back(lines[i]);
    }

    return join_lines(result);
}

// ── Diagnostic grouping ─────────────────────────────────────────

// Extract the diagnostic message from a compiler line, stripping location.
// E.g. "src/foo.cpp:10:5: warning: unused variable 'x'" -> "warning: unused variable 'x'"
// Returns empty if the line doesn't look like a diagnostic.
static std::string extract_diagnostic_key(const std::string& line) {
    // Match patterns like "file:line:col: type: message" or "file(line): type: message"
    // Look for "error:" or "warning:" after a location prefix
    for (const char* tag : {"error:", "warning:", "note:"}) {
        auto pos = line.find(tag);
        if (pos != std::string::npos && pos > 0) {
            return line.substr(pos);
        }
    }
    return "";
}

// Extract just the file path from a diagnostic line.
static std::string extract_diagnostic_file(const std::string& line) {
    // "src/foo.cpp:10:5: ..." or "src/foo.cpp(10): ..."
    auto colon = line.find(':');
    auto paren = line.find('(');
    size_t end = std::string::npos;
    if (colon != std::string::npos && paren != std::string::npos) {
        end = std::min(colon, paren);
    } else if (colon != std::string::npos) {
        end = colon;
    } else if (paren != std::string::npos) {
        end = paren;
    }
    if (end != std::string::npos && end > 0) {
        return line.substr(0, end);
    }
    return "";
}

std::string group_diagnostics(const std::string& output) {
    auto lines = split_lines(output);
    if (lines.size() < 5) return output;  // too short to benefit

    // First pass: count each unique diagnostic message
    struct DiagInfo {
        std::string first_line;  // full line of first occurrence
        std::vector<std::string> files;  // files where it appears
        uint32_t count = 0;
    };
    std::unordered_map<std::string, DiagInfo> diag_counts;
    std::vector<std::string> diag_order;  // preserve first-seen order

    for (const auto& line : lines) {
        std::string key = extract_diagnostic_key(line);
        if (key.empty()) continue;

        auto it = diag_counts.find(key);
        if (it == diag_counts.end()) {
            DiagInfo info;
            info.first_line = line;
            info.count = 1;
            std::string file = extract_diagnostic_file(line);
            if (!file.empty()) info.files.push_back(file);
            diag_counts[key] = std::move(info);
            diag_order.push_back(key);
        } else {
            it->second.count++;
            std::string file = extract_diagnostic_file(line);
            if (!file.empty() && it->second.files.size() < 5) {
                // Avoid duplicate file names
                bool found = false;
                for (const auto& f : it->second.files) {
                    if (f == file) { found = true; break; }
                }
                if (!found) it->second.files.push_back(file);
            }
        }
    }

    // If no grouping benefit (all unique), return as-is
    bool has_duplicates = false;
    for (const auto& key : diag_order) {
        if (diag_counts[key].count > 1) { has_duplicates = true; break; }
    }
    if (!has_duplicates) return output;

    // Second pass: rebuild output, replacing duplicate diagnostics with grouped form
    std::unordered_set<std::string> emitted;
    std::vector<std::string> result;

    for (const auto& line : lines) {
        std::string key = extract_diagnostic_key(line);
        if (key.empty()) {
            // Non-diagnostic line — keep as-is
            result.push_back(line);
            continue;
        }

        if (emitted.count(key) > 0) continue;  // already grouped
        emitted.insert(key);

        const auto& info = diag_counts[key];
        if (info.count == 1) {
            result.push_back(info.first_line);
        } else {
            result.push_back(info.first_line);
            std::string summary = "  (" + std::to_string(info.count - 1) + " more";
            if (info.files.size() > 1) {
                summary += " in ";
                for (size_t i = 1; i < info.files.size() && i < 4; i++) {
                    if (i > 1) summary += ", ";
                    summary += info.files[i];
                }
                if (info.files.size() > 4) {
                    summary += ", ...";
                }
            }
            summary += ")";
            result.push_back(summary);
        }
    }

    return join_lines(result);
}

// ── Noise directory filtering ───────────────────────────────────

static const char* const noise_dirs[] = {
    "node_modules", ".git", "__pycache__", ".next",
    ".cache", ".turbo", ".vercel", ".pytest_cache", ".mypy_cache",
    ".tox", ".venv", "venv", ".env", "coverage", ".nyc_output",
    ".DS_Store", "Thumbs.db", ".idea", ".vscode", ".vs",
};

static bool is_noise_path(const std::string& line) {
    for (const char* dir : noise_dirs) {
        size_t dir_len = strlen(dir);
        // Find all occurrences of the noise dir name in the line
        size_t pos = 0;
        while ((pos = line.find(dir, pos)) != std::string::npos) {
            // Check boundary: must be preceded by non-alnum (/, space, tree decoration, start)
            bool left_ok = (pos == 0) ||
                           !std::isalnum(static_cast<unsigned char>(line[pos - 1]));
            // Check boundary: must be followed by non-alnum (/, end, space, tree decoration)
            size_t end = pos + dir_len;
            bool right_ok = (end == line.size()) ||
                            !std::isalnum(static_cast<unsigned char>(line[end]));
            if (left_ok && right_ok) return true;
            pos = end;
        }
    }
    return false;
}

std::string filter_noise_dirs(const std::string& output) {
    auto lines = split_lines(output);
    std::vector<std::string> kept;
    uint32_t stripped = 0;

    for (const auto& line : lines) {
        if (is_noise_path(line)) {
            stripped++;
        } else {
            kept.push_back(line);
        }
    }

    if (stripped == 0) return output;

    std::string result = join_lines(kept);
    result += "\n[" + std::to_string(stripped) + " noise entries stripped]";
    return result;
}

} // namespace ptrclaw
