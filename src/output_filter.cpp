#include "output_filter.hpp"
#include <sstream>

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

} // namespace ptrclaw
