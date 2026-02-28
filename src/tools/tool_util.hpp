#pragma once
#include "../tool.hpp"
#include <nlohmann/json.hpp>
#include <optional>

namespace ptrclaw {

// Parse JSON tool arguments. Returns error ToolResult on failure.
inline std::optional<ToolResult> parse_tool_json(
    const std::string& args_json, nlohmann::json& out) {
    try {
        out = nlohmann::json::parse(args_json);
    } catch (const std::exception& e) {
        return ToolResult{false, std::string("Failed to parse arguments: ") + e.what()};
    }
    return std::nullopt;
}

// Check that a required string field exists. Returns error ToolResult if missing.
inline std::optional<ToolResult> require_string(const nlohmann::json& args, const char* field) {
    if (!args.contains(field) || !args[field].is_string()) {
        return ToolResult{false, std::string("Missing required parameter: ") + field};
    }
    return std::nullopt;
}

// Reject paths containing ".." to prevent directory traversal.
inline std::optional<ToolResult> validate_safe_path(const std::string& path) {
    if (path.find("..") != std::string::npos) {
        return ToolResult{false, "Path must not contain '..'"};
    }
    return std::nullopt;
}

// Memory tool preamble: check memory enabled + parse JSON.
class Memory;
inline std::optional<ToolResult> parse_memory_tool_args(
    Memory* memory, const std::string& args_json, nlohmann::json& out) {
    if (!memory) return ToolResult{false, "Memory system is not enabled"};
    return parse_tool_json(args_json, out);
}

} // namespace ptrclaw
