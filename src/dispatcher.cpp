#include "dispatcher.hpp"
#include "util.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>

namespace ptrclaw {

std::vector<ToolCall> parse_xml_tool_calls(const std::string& text) {
    std::vector<ToolCall> calls;
    const std::string open_tag = "<tool_call>";
    const std::string close_tag = "</tool_call>";

    size_t pos = 0;
    while (pos < text.size()) {
        size_t start = text.find(open_tag, pos);
        if (start == std::string::npos) break;

        size_t content_start = start + open_tag.size();
        size_t end = text.find(close_tag, content_start);
        if (end == std::string::npos) break;

        std::string content = trim(text.substr(content_start, end - content_start));
        std::string repaired = repair_json(content);
        pos = end + close_tag.size();

        try {
            nlohmann::json j = nlohmann::json::parse(repaired);

            ToolCall call;
            call.id = generate_id();
            call.name = j.value("name", "");

            if (j.contains("arguments")) {
                if (j["arguments"].is_object() || j["arguments"].is_array()) {
                    call.arguments = j["arguments"].dump();
                } else {
                    call.arguments = j["arguments"].get<std::string>();
                }
            } else {
                call.arguments = "{}";
            }

            if (!call.name.empty()) {
                calls.push_back(std::move(call));
            }
        } catch (const std::exception&) { // NOLINT(bugprone-empty-catch)
            // Malformed tool call JSON — skip and try remaining calls
        }
    }

    return calls;
}

std::string repair_json(const std::string& json_str) {
    std::string s = json_str;

    // Balance braces
    int brace_count = 0;
    int bracket_count = 0;
    for (char c : s) {
        if (c == '{') brace_count++;
        else if (c == '}') brace_count--;
        else if (c == '[') bracket_count++;
        else if (c == ']') bracket_count--;
    }

    // Append missing closing braces/brackets
    while (bracket_count > 0) {
        s += ']';
        bracket_count--;
    }
    while (brace_count > 0) {
        s += '}';
        brace_count--;
    }

    // Remove trailing commas before } or ]
    // Search for patterns like ,\s*} or ,\s*]
    std::string result;
    result.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == ',') {
            // Look ahead past whitespace for } or ]
            size_t j = i + 1;
            while (j < s.size() && (s[j] == ' ' || s[j] == '\t' || s[j] == '\n' || s[j] == '\r')) {
                j++;
            }
            if (j < s.size() && (s[j] == '}' || s[j] == ']')) {
                // Skip the comma
                continue;
            }
        }
        result += s[i];
    }

    // Try to parse; if fails, return original
    try {
        (void)nlohmann::json::parse(result);
        return result;
    } catch (const std::exception&) {
        return json_str;
    }
}

ToolResult dispatch_tool(const ToolCall& call,
                         const std::vector<std::unique_ptr<Tool>>& tools) {
    for (const auto& tool : tools) {
        if (tool->tool_name() == call.name) {
            return tool->execute(call.arguments);
        }
    }
    return ToolResult{false, "Unknown tool: " + call.name};
}

std::string format_tool_results_xml(const std::string& tool_name,
                                    bool success,
                                    const std::string& output) {
    std::string status = success ? "ok" : "error";
    return "<tool_result name=\"" + tool_name + "\" status=\"" + status + "\">" +
           output + "</tool_result>";
}

ChatMessage format_tool_result_message(const std::string& tool_call_id,
                                       const std::string& tool_name,
                                       bool success,
                                       const std::string& output) {
    std::string content = success ? output : "Error: " + output;
    return ChatMessage{Role::Tool, content, tool_name, tool_call_id};
}

} // namespace ptrclaw
