#include "memory_store.hpp"
#include "tool_util.hpp"
#include "../plugin.hpp"
#include <nlohmann/json.hpp>

static ptrclaw::ToolRegistrar reg_memory_store("memory_store",
    []() { return std::make_unique<ptrclaw::MemoryStoreTool>(); });

namespace ptrclaw {

ToolResult MemoryStoreTool::execute(const std::string& args_json) {
    nlohmann::json args;
    if (auto err = parse_memory_tool_args(memory_, args_json, args)) return *err;
    if (auto err = require_string(args, "key")) return *err;
    if (auto err = require_string(args, "content")) return *err;

    std::string key = args["key"].get<std::string>();
    std::string content = args["content"].get<std::string>();

    std::string cat_str = "knowledge";
    if (args.contains("category") && args["category"].is_string()) {
        cat_str = args["category"].get<std::string>();
    }
    MemoryCategory category = category_from_string(cat_str);

    std::string session_id;
    if (args.contains("session_id") && args["session_id"].is_string()) {
        session_id = args["session_id"].get<std::string>();
    }

    std::string id = memory_->store(key, content, category, session_id);

    // Create links if specified
    if (args.contains("links") && args["links"].is_array()) {
        for (const auto& lnk : args["links"]) {
            if (lnk.is_string()) {
                memory_->link(key, lnk.get<std::string>());
            }
        }
    }

    return ToolResult{true, "Stored memory '" + key + "' (id: " + id + ")"};
}

std::string MemoryStoreTool::description() const {
    return "Store or update a memory entry for later recall";
}

std::string MemoryStoreTool::parameters_json() const {
    return R"json({"type":"object","properties":{"key":{"type":"string","description":"Human-readable key for this memory (unique, upserts on conflict)"},"content":{"type":"string","description":"The content to remember"},"category":{"type":"string","enum":["core","knowledge","conversation"],"description":"Memory category (default: knowledge)"},"session_id":{"type":"string","description":"Optional session ID for scoping"},"links":{"type":"array","items":{"type":"string"},"description":"Optional keys of existing entries to link to"}},"required":["key","content"]})json";
}

} // namespace ptrclaw
