#include "memory_forget.hpp"
#include "memory_tool_util.hpp"
#include "../plugin.hpp"
#include <nlohmann/json.hpp>

static ptrclaw::ToolRegistrar reg_memory_forget("memory_forget",
    []() { return std::make_unique<ptrclaw::MemoryForgetTool>(); });

namespace ptrclaw {

ToolResult MemoryForgetTool::execute(const std::string& args_json) {
    nlohmann::json args;
    if (auto err = parse_memory_tool_args(memory_, args_json, args)) return *err;
    if (auto err = require_string(args, "key")) return *err;

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
