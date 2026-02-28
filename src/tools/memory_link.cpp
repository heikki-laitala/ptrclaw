#include "memory_link.hpp"
#include "tool_util.hpp"
#include "../plugin.hpp"
#include <nlohmann/json.hpp>

static ptrclaw::ToolRegistrar reg_memory_link("memory_link",
    []() { return std::make_unique<ptrclaw::MemoryLinkTool>(); });

namespace ptrclaw {

ToolResult MemoryLinkTool::execute(const std::string& args_json) {
    nlohmann::json args;
    if (auto err = parse_memory_tool_args(memory_, args_json, args)) return *err;
    if (auto err = require_string(args, "action")) return *err;
    if (auto err = require_string(args, "from")) return *err;
    if (auto err = require_string(args, "to")) return *err;

    std::string action = args["action"].get<std::string>();
    std::string from = args["from"].get<std::string>();
    std::string to = args["to"].get<std::string>();

    if (action == "link") {
        bool ok = memory_->link(from, to);
        if (!ok) return ToolResult{false, "Failed to link: one or both entries not found"};
        return ToolResult{true, "Linked '" + from + "' <-> '" + to + "'"};
    }
    if (action == "unlink") {
        bool ok = memory_->unlink(from, to);
        if (!ok) return ToolResult{false, "Failed to unlink: link does not exist"};
        return ToolResult{true, "Unlinked '" + from + "' <-> '" + to + "'"};
    }

    return ToolResult{false, "Unknown action: " + action + " (use 'link' or 'unlink')"};
}

std::string MemoryLinkTool::description() const {
    return "Create or remove bidirectional links between memory entries";
}

std::string MemoryLinkTool::parameters_json() const {
    return R"json({"type":"object","properties":{"action":{"type":"string","enum":["link","unlink"],"description":"Whether to create or remove a link"},"from":{"type":"string","description":"Key of the first memory entry"},"to":{"type":"string","description":"Key of the second memory entry"}},"required":["action","from","to"]})json";
}

} // namespace ptrclaw
