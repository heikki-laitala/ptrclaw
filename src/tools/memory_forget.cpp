#include "memory_forget.hpp"
#include "../plugin.hpp"
#include <nlohmann/json.hpp>

static ptrclaw::ToolRegistrar reg_memory_forget("memory_forget",
    []() { return std::make_unique<ptrclaw::MemoryForgetTool>(); });

namespace ptrclaw {

ToolResult MemoryForgetTool::execute(const std::string& args_json) {
    if (!memory_) {
        return ToolResult{false, "Memory system is not enabled"};
    }

    nlohmann::json args;
    try {
        args = nlohmann::json::parse(args_json);
    } catch (const std::exception& e) {
        return ToolResult{false, std::string("Failed to parse arguments: ") + e.what()};
    }

    if (!args.contains("key") || !args["key"].is_string()) {
        return ToolResult{false, "Missing required parameter: key"};
    }

    std::string key = args["key"].get<std::string>();
    bool deleted = memory_->forget(key);

    if (deleted) {
        return ToolResult{true, "Forgot memory '" + key + "'"};
    }
    return ToolResult{false, "No memory found with key '" + key + "'"};
}

std::string MemoryForgetTool::description() const {
    return "Delete a stored memory entry by key";
}

std::string MemoryForgetTool::parameters_json() const {
    return R"({"type":"object","properties":{"key":{"type":"string","description":"The key of the memory to forget"}},"required":["key"]})";
}

} // namespace ptrclaw
