#include "output_filter.hpp"
#include "util.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
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
    GitDiff, GitStatus, GitLog, GitOps, TestRunner, BuildLog, DirListing,
    Linter, SearchResult, HttpResponse, ContainerOps, PackageManager,
    GitHubCli, EnvVars, DepFile, Other
};

static bool starts_with(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

static bool contains(const std::string& s, const std::string& needle) {
    return s.find(needle) != std::string::npos;
}

static bool contains_icase(const std::string& s, const std::string& needle) {
    if (needle.size() > s.size()) return false;
    auto it = std::search(s.begin(), s.end(), needle.begin(), needle.end(),
                          [](char a, char b) {
                              return std::tolower(static_cast<unsigned char>(a)) ==
                                     std::tolower(static_cast<unsigned char>(b));
                          });
    return it != s.end();
}

static ShellCommandType classify_command(const std::string& command) {
    // Trim leading whitespace / env vars for matching
    std::string cmd = command;
    size_t start = cmd.find_first_not_of(" \t");
    if (start != std::string::npos) cmd = cmd.substr(start);

    if (starts_with(cmd, "git diff") || contains(cmd, "| git diff")) return ShellCommandType::GitDiff;
    if (starts_with(cmd, "git status")) return ShellCommandType::GitStatus;
    if (starts_with(cmd, "git log") || starts_with(cmd, "git shortlog")) return ShellCommandType::GitLog;
    // Git operations (add, commit, push, pull, fetch, clone, merge, rebase, checkout)
    if (starts_with(cmd, "git add") || starts_with(cmd, "git commit") ||
        starts_with(cmd, "git push") || starts_with(cmd, "git pull") ||
        starts_with(cmd, "git fetch") || starts_with(cmd, "git clone") ||
        starts_with(cmd, "git merge") || starts_with(cmd, "git rebase") ||
        starts_with(cmd, "git checkout") || starts_with(cmd, "git switch")) {
        return ShellCommandType::GitOps;
    }

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

    // Linters (after build tools — cargo clippy is caught above, which is fine)
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

    // GitHub CLI
    if (starts_with(cmd, "gh ")) {
        return ShellCommandType::GitHubCli;
    }

    // Environment variables
    if (starts_with(cmd, "env") || starts_with(cmd, "printenv") ||
        starts_with(cmd, "set ") || starts_with(cmd, "export")) {
        return ShellCommandType::EnvVars;
    }

    // Dependency file reads (cat package.json, cat Cargo.toml, etc.)
    if (starts_with(cmd, "cat ") &&
        (contains(cmd, "package.json") || contains(cmd, "Cargo.toml") ||
         contains(cmd, "requirements.txt") || contains(cmd, "pyproject.toml") ||
         contains(cmd, "go.mod") || contains(cmd, "Gemfile") ||
         contains(cmd, "build.gradle") || contains(cmd, "pom.xml"))) {
        return ShellCommandType::DepFile;
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
    if (lines.size() > 3) {
        size_t tail_start = lines.size() - 3;
        for (size_t i = tail_start; i < lines.size(); i++) {
            if (!lines[i].empty() &&
                std::find(kept.begin(), kept.end(), lines[i]) == kept.end()) {
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

    // Group repeated diagnostics, then strip remaining noise
    std::string grouped = group_diagnostics(output);
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
// Shortens long paths and centers match content in a window.

// Shorten long paths: src/components/deeply/nested/file.cpp -> src/.../nested/file.cpp
static std::string compact_path(const std::string& path) {
    if (path.size() <= 50) return path;
    auto last_slash = path.rfind('/');
    if (last_slash == std::string::npos || last_slash == 0) return path;
    auto second_last = path.rfind('/', last_slash - 1);
    if (second_last == std::string::npos || second_last == 0) return path;
    auto first_slash = path.find('/');
    if (first_slash >= second_last) return path;
    return path.substr(0, first_slash + 1) + "..." +
           path.substr(second_last);
}

// Truncate match content to ~80 chars, centering around the non-path portion.
static std::string truncate_match_content(const std::string& line, size_t content_start) {
    constexpr size_t max_content = 80;
    if (line.size() - content_start <= max_content) return line;
    std::string prefix = line.substr(0, content_start);
    std::string content = line.substr(content_start);
    if (content.size() > max_content) {
        content = content.substr(0, max_content) + "...";
    }
    return prefix + content;
}

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
    result.reserve(non_match_lines.size() + file_order.size());
    for (const auto& nl : non_match_lines) {
        result.push_back(nl);
    }

    for (const auto& file : file_order) {
        const auto& indices = file_matches[file];
        std::string short_path = compact_path(file);
        uint32_t shown = 0;
        for (size_t idx : indices) {
            if (shown < max_matches_per_file) {
                const auto& line = lines[idx];
                // Rebuild line with shortened path + truncated content
                auto colon1 = line.find(':');
                std::string rebuilt = short_path + line.substr(colon1);
                result.push_back(truncate_match_content(rebuilt, short_path.size()));
                shown++;
            }
        }
        if (indices.size() > max_matches_per_file) {
            result.push_back("  [..." + std::to_string(indices.size() - max_matches_per_file) +
                             " more matches in " + short_path + "]");
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

    // Emit stripped-header summary and transition to body mode
    auto end_headers = [&]() {
        in_headers = false;
        past_headers = true;
        if (header_count > 2) {
            kept.push_back("[" + std::to_string(header_count - 2) + " headers stripped]");
        }
    };

    for (const auto& line : lines) {
        // curl -v: lines starting with > or * are request headers / connection info
        if (starts_with(line, "> ") || starts_with(line, "* ") ||
            line == ">" || line == "*") {
            continue;
        }

        if (starts_with(line, "< ") || line == "<") {
            if (line == "<") {
                end_headers();
                continue;
            }
            // Response header — keep status line and content-type only
            std::string hdr = line.substr(2);
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
            end_headers();
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

// ── Git operations filter ───────────────────────────────────────
// Compress verbose git add/commit/push/pull/fetch/clone output.

static std::string filter_git_ops(const std::string& output) {
    auto lines = split_lines(output);
    std::vector<std::string> kept;

    for (const auto& line : lines) {
        // Skip progress indicators from push/pull/fetch/clone
        if (contains(line, "Enumerating objects:") ||
            contains(line, "Counting objects:") ||
            contains(line, "Compressing objects:") ||
            contains(line, "Writing objects:") ||
            contains(line, "Receiving objects:") ||
            contains(line, "Resolving deltas:") ||
            contains(line, "remote: Counting") ||
            contains(line, "remote: Compressing") ||
            contains(line, "remote: Total")) {
            continue;
        }
        // Skip npm-style progress lines
        if (contains(line, "Unpacking objects:")) continue;
        kept.push_back(line);
    }

    return join_lines(kept);
}

// ── GitHub CLI filter ───────────────────────────────────────────
// Compact gh pr/issue/run list output.

static std::string filter_github_cli(const std::string& output) {
    auto lines = split_lines(output);
    if (lines.size() < 3) return output;

    std::vector<std::string> kept;
    constexpr uint32_t max_items = 15;
    uint32_t item_count = 0;

    for (const auto& line : lines) {
        // Truncate wide table rows (gh outputs are tab-separated)
        if (line.size() > 150) {
            kept.push_back(line.substr(0, 150) + "...");
        } else {
            kept.push_back(line);
        }
        item_count++;
        if (item_count > max_items + 1) { // +1 for header row
            kept.push_back("[..." + std::to_string(lines.size() - max_items - 1) + " more items]");
            break;
        }
    }

    return join_lines(kept);
}

// ── Environment variable filter ─────────────────────────────────
// Strips noisy env vars, keeps project-relevant ones.

static std::string filter_env_vars(const std::string& output) {
    auto lines = split_lines(output);
    if (lines.size() < 10) return output;

    // Noisy env var prefixes to strip
    static const char* const noise_prefixes[] = {
        "LS_COLORS=", "LESS_TERMCAP_", "LSCOLORS=",
        "TERMCAP=", "PS1=", "PS2=", "PROMPT_COMMAND=",
        "BASH_FUNC_", "COMP_", "FIGNORE=",
        "_=", "SHLVL=", "OLDPWD=", "PWD=",
        "COLORTERM=", "TERM_PROGRAM_VERSION=",
    };

    std::vector<std::string> kept;
    uint32_t stripped = 0;

    for (const auto& line : lines) {
        bool is_noise = false;
        for (const char* prefix : noise_prefixes) {
            if (starts_with(line, prefix)) {
                is_noise = true;
                break;
            }
        }
        // Also strip very long values (e.g., PATH with 50+ entries)
        if (!is_noise && line.size() > 200) {
            auto eq = line.find('=');
            if (eq < 40) {  // npos is always > 40
                kept.push_back(line.substr(0, eq + 1) + line.substr(eq + 1, 100) + "...");
                continue;
            }
        }
        if (is_noise) {
            stripped++;
        } else {
            kept.push_back(line);
        }
    }

    if (stripped > 0) {
        kept.push_back("[" + std::to_string(stripped) + " noise env vars stripped]");
    }

    return join_lines(kept);
}

// ── Dependency file summarizer ──────────────────────────────────
// Summarize package.json, Cargo.toml, requirements.txt, etc.

static std::string summarize_dep_file(const std::string& output) {
    auto lines = split_lines(output);
    if (lines.size() < 15) return output;

    // Detect format and summarize
    bool is_json = !output.empty() && (output[0] == '{' || output[0] == '[');
    bool is_toml = false;
    bool is_requirements = false;

    for (const auto& line : lines) {
        if (starts_with(line, "[dependencies]") || starts_with(line, "[package]")) {
            is_toml = true;
            break;
        }
        // requirements.txt: lines like "package==version" or "package>=version"
        if (contains(line, "==") || (contains(line, ">=") && !contains(line, "{"))) {
            is_requirements = true;
        }
    }

    if (is_json) {
        // Try JSON schema extraction for package.json
        std::string schema = extract_json_schema(output);
        if (!schema.empty()) return schema;
    }

    if (is_toml) {
        // Show section headers + first N entries per section
        std::vector<std::string> kept;
        constexpr uint32_t max_per_section = 10;
        uint32_t section_count = 0;
        uint32_t hidden = 0;

        for (const auto& line : lines) {
            if (starts_with(line, "[")) {
                section_count = 0;
                kept.push_back(line);
                continue;
            }
            if (section_count < max_per_section || line.empty()) {
                kept.push_back(line);
                section_count++;
            } else {
                hidden++;
            }
        }
        if (hidden > 0) {
            kept.push_back("[" + std::to_string(hidden) + " more entries hidden]");
        }
        return join_lines(kept);
    }

    if (is_requirements) {
        // Show first 15 packages
        constexpr uint32_t max_pkgs = 15;
        if (lines.size() <= max_pkgs) return output;
        std::vector<std::string> kept(lines.begin(), lines.begin() + max_pkgs);
        kept.push_back("[..." + std::to_string(lines.size() - max_pkgs) + " more packages]");
        return join_lines(kept);
    }

    // go.mod or unknown: show first 20 lines
    constexpr uint32_t max_lines_dep = 20;
    if (lines.size() <= max_lines_dep) return output;
    std::vector<std::string> kept(lines.begin(), lines.begin() + max_lines_dep);
    kept.push_back("[..." + std::to_string(lines.size() - max_lines_dep) + " more lines]");
    return join_lines(kept);
}

// ── Diff file-level summary ─────────────────────────────────────
// For very long diffs, add per-file +/-/~ summary at the top.

static std::string enhance_git_diff(const std::string& filtered_diff) {
    auto lines = split_lines(filtered_diff);
    // Only enhance if diff is large
    if (lines.size() < 50) return filtered_diff;

    // Count changes per file
    struct FileStats {
        uint32_t added = 0;
        uint32_t removed = 0;
    };
    std::vector<std::string> file_order;
    std::unordered_map<std::string, FileStats> stats;
    std::string current_file;

    for (const auto& line : lines) {
        if (starts_with(line, "diff --git")) {
            // Extract file name: "diff --git a/foo b/foo" -> "foo"
            auto b_pos = line.find(" b/");
            if (b_pos != std::string::npos) {
                current_file = line.substr(b_pos + 3);
                if (stats.find(current_file) == stats.end()) {
                    file_order.push_back(current_file);
                    stats[current_file] = {};
                }
            }
        } else if (!current_file.empty()) {
            if (!line.empty() && line[0] == '+' && !starts_with(line, "+++")) {
                stats[current_file].added++;
            } else if (!line.empty() && line[0] == '-' && !starts_with(line, "---")) {
                stats[current_file].removed++;
            }
        }
    }

    if (file_order.size() < 2) return filtered_diff;

    // Prepend summary
    std::string summary = "Files changed:\n";
    for (const auto& file : file_order) {
        const auto& s = stats[file];
        summary += "  " + file + " (+" + std::to_string(s.added) +
                   "/-" + std::to_string(s.removed) + ")\n";
    }
    summary += "\n";

    return summary + filtered_diff;
}

// ── npm/pnpm boilerplate stripping ──────────────────────────────
// Called within the PackageManager filter for npm/pnpm commands.

static std::string strip_npm_boilerplate(const std::string& output) {
    auto lines = split_lines(output);
    std::vector<std::string> kept;

    for (const auto& line : lines) {
        // Strip script header lines: "> project@version script"
        if (starts_with(line, "> ") && contains(line, "@")) continue;
        // Strip npm WARN/notice
        if (starts_with(line, "npm WARN") || starts_with(line, "npm notice")) continue;
        // Strip pnpm progress indicators
        if (contains(line, "Progress:") || contains(line, "Packages:")) continue;
        // Skip empty lines that follow stripped content
        if (line.empty() && !kept.empty() && kept.back().empty()) continue;
        kept.push_back(line);
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
        case ShellCommandType::GitDiff:
            filtered = filter_git_diff(cleaned);
            filtered = enhance_git_diff(filtered);
            break;
        case ShellCommandType::GitStatus:  filtered = filter_git_status(cleaned);  break;
        case ShellCommandType::GitLog:     filtered = cleaned; break;
        case ShellCommandType::GitOps:     filtered = filter_git_ops(cleaned);     break;
        case ShellCommandType::TestRunner: filtered = filter_test_output(cleaned); break;
        case ShellCommandType::BuildLog:
            filtered = filter_build_log(cleaned);
            filtered = group_diagnostics(filtered);
            break;
        case ShellCommandType::DirListing:     filtered = filter_noise_dirs(cleaned);       break;
        case ShellCommandType::Linter:         filtered = filter_linter_output(cleaned);    break;
        case ShellCommandType::SearchResult:   filtered = filter_search_results(cleaned);   break;
        case ShellCommandType::HttpResponse:   filtered = filter_http_response(cleaned);    break;
        case ShellCommandType::ContainerOps:   filtered = filter_container_output(cleaned); break;
        case ShellCommandType::PackageManager:
            filtered = strip_npm_boilerplate(cleaned);
            filtered = filter_package_output(filtered);
            break;
        case ShellCommandType::GitHubCli:      filtered = filter_github_cli(cleaned);       break;
        case ShellCommandType::EnvVars:        filtered = filter_env_vars(cleaned);          break;
        case ShellCommandType::DepFile:        filtered = summarize_dep_file(cleaned);       break;
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

bool is_sensitive_command(const std::string& command) {
    // Reuse classifier for env/printenv/export detection
    if (classify_command(command) == ShellCommandType::EnvVars) return true;

    // Auth/token/secret-related commands not caught by classifier
    if (contains_icase(command, "token") || contains_icase(command, "secret") ||
        contains_icase(command, "password") || contains_icase(command, "credential") ||
        contains_icase(command, "auth") || contains_icase(command, "login")) {
        return true;
    }
    // .env files
    if (contains(command, ".env")) return true;

    return false;
}

// Redact lines that look like key=value secrets
static std::string redact_sensitive_output(const std::string& output) {
    auto lines = split_lines(output);
    std::vector<std::string> result;

    for (const auto& line : lines) {
        auto eq = line.find('=');
        if (eq != std::string::npos && eq > 0 && eq < line.size() - 1) {
            std::string key = line.substr(0, eq);
            if (contains_icase(key, "key") || contains_icase(key, "secret") ||
                contains_icase(key, "token") || contains_icase(key, "password") ||
                contains_icase(key, "credential") || contains_icase(key, "auth")) {
                result.push_back(key + "=[REDACTED]");
                continue;
            }
        }
        result.push_back(line);
    }

    return join_lines(result);
}

std::string tee_shell_output(const std::string& command,
                             const std::string& output,
                             const std::string& tee_dir) {
    if (output.empty()) return "";

    std::string dir = tee_dir.empty() ? expand_home("~/.ptrclaw/tee") : tee_dir;

    try {
        std::filesystem::create_directories(dir);
    } catch (...) {
        return "";
    }

    // Redact sensitive commands before writing to disk
    std::string safe_output = is_sensitive_command(command)
        ? redact_sensitive_output(output)
        : output;

    std::string filename = std::to_string(epoch_seconds()) + "_" + generate_id() + ".log";
    std::string path = dir + "/" + filename;

    if (atomic_write_file(path, safe_output)) {
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
            } catch (...) { any_oversized = true; break; }
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

// Hand-rolled character classifiers (avoid <regex> which adds ~300 KB)
static bool is_hex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
static bool is_digit(char c) { return c >= '0' && c <= '9'; }
static bool is_word(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}
static bool is_path_char(char c) {
    return is_word(c) || c == '/' || c == '.' || c == '-';
}

// Match exactly N hex chars at pos, return true if matched
static bool match_hex_n(const std::string& s, size_t pos, size_t n) {
    if (pos + n > s.size()) return false;
    for (size_t i = 0; i < n; i++) {
        if (!is_hex(s[pos + i])) return false;
    }
    return true;
}

// Try to match a timestamp at pos: YYYY[-/]MM[-/]DD[T ]HH:MM:SS[.,]digits
// Returns length consumed, or 0 if no match.
static size_t match_timestamp(const std::string& s, size_t pos) {
    // Need at least "YYYY-MM-DDTHH:MM:SS" = 19 chars
    if (pos + 19 > s.size()) return 0;
    size_t i = pos;
    // YYYY
    for (int k = 0; k < 4; k++) { if (!is_digit(s[i])) return 0; i++; }
    if (s[i] != '-' && s[i] != '/') return 0; i++;
    // MM
    for (int k = 0; k < 2; k++) { if (!is_digit(s[i])) return 0; i++; }
    if (s[i] != '-' && s[i] != '/') return 0; i++;
    // DD
    for (int k = 0; k < 2; k++) { if (!is_digit(s[i])) return 0; i++; }
    if (s[i] != 'T' && s[i] != ' ') return 0; i++;
    // HH:MM:SS
    for (int k = 0; k < 2; k++) { if (!is_digit(s[i])) return 0; i++; }
    if (s[i] != ':') return 0; i++;
    for (int k = 0; k < 2; k++) { if (!is_digit(s[i])) return 0; i++; }
    if (s[i] != ':') return 0; i++;
    for (int k = 0; k < 2; k++) { if (!is_digit(s[i])) return 0; i++; }
    // Optional fractional: [.,]digits
    if (i < s.size() && (s[i] == '.' || s[i] == ',')) {
        i++;
        while (i < s.size() && is_digit(s[i])) i++;
    }
    // Consume trailing whitespace
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    return i - pos;
}

// Try to match UUID at pos: 8-4-4-4-12 hex
static size_t match_uuid(const std::string& s, size_t pos) {
    // 8-4-4-4-12 = 36 chars
    if (pos + 36 > s.size()) return 0;
    size_t i = pos;
    if (!match_hex_n(s, i, 8)) return 0; i += 8;
    if (s[i] != '-') return 0; i++;
    if (!match_hex_n(s, i, 4)) return 0; i += 4;
    if (s[i] != '-') return 0; i++;
    if (!match_hex_n(s, i, 4)) return 0; i += 4;
    if (s[i] != '-') return 0; i++;
    if (!match_hex_n(s, i, 4)) return 0; i += 4;
    if (s[i] != '-') return 0; i++;
    if (!match_hex_n(s, i, 12)) return 0;
    return 36;
}

// Normalize a log line by stripping timestamps, UUIDs, hex, and large numbers.
// Uses hand-written matchers instead of std::regex to avoid binary bloat.
static std::string normalize_log_line(const std::string& line) {
    std::string out;
    out.reserve(line.size());
    size_t i = 0;

    while (i < line.size()) {
        // 1. Timestamps: YYYY-MM-DDTHH:MM:SS...
        if (is_digit(line[i])) {
            size_t ts_len = match_timestamp(line, i);
            if (ts_len > 0) { i += ts_len; continue; }

            // 2. UUID: 8-4-4-4-12 hex (check before generic hex/number)
            size_t uuid_len = match_uuid(line, i);
            if (uuid_len > 0) { out += "<UUID>"; i += uuid_len; continue; }
        }

        // 3. Hex literals: 0x[0-9a-fA-F]+
        if (line[i] == '0' && i + 2 < line.size() && line[i + 1] == 'x' && is_hex(line[i + 2])) {
            out += "<HEX>";
            i += 2;
            while (i < line.size() && is_hex(line[i])) i++;
            continue;
        }

        // 4. Large numbers: 4+ digits at word boundary
        if (is_digit(line[i]) && (i == 0 || !is_word(line[i - 1]))) {
            size_t start = i;
            while (i < line.size() && is_digit(line[i])) i++;
            bool at_boundary = (i >= line.size() || !is_word(line[i]));
            if (at_boundary && (i - start) >= 4) {
                out += "<N>";
            } else {
                out.append(line, start, i - start);
            }
            continue;
        }

        // 5. Absolute paths: /word-path-chars+
        if (line[i] == '/' && i + 1 < line.size() && is_path_char(line[i + 1])) {
            out += "<PATH>";
            i++;
            while (i < line.size() && is_path_char(line[i])) i++;
            continue;
        }

        out += line[i];
        i++;
    }
    return out;
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
