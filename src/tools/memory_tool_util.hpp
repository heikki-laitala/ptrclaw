#pragma once
#include "../memory.hpp"
#include <nlohmann/json.hpp>
#include <optional>

namespace ptrclaw {

// Common preamble for memory tool execute(): check memory and parse JSON args.
// Returns a ToolResult error on failure, or std::nullopt on success (args populated).
inline std::optional<ToolResult> parse_memory_tool_args(
    Memory* memory, const std::string& args_json, nlohmann::json& out) {
    if (!memory) return ToolResult{false, "Memory system is not enabled"};
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

} // namespace ptrclaw
