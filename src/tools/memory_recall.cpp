#include "memory_recall.hpp"
#include "tool_util.hpp"
#include "../plugin.hpp"
#include <nlohmann/json.hpp>
#include <sstream>

static ptrclaw::ToolRegistrar reg_memory_recall("memory_recall",
    []() { return std::make_unique<ptrclaw::MemoryRecallTool>(); });

namespace ptrclaw {

ToolResult MemoryRecallTool::execute(const std::string& args_json) {
    nlohmann::json args;
    if (auto err = parse_memory_tool_args(memory_, args_json, args)) return *err;
    if (auto err = require_string(args, "query")) return *err;

    std::string query = args["query"].get<std::string>();

    uint32_t limit = 5;
    if (args.contains("limit") && args["limit"].is_number_unsigned()) {
        limit = args["limit"].get<uint32_t>();
    }

    std::optional<MemoryCategory> cat_filter;
    auto cat_str = get_optional_string(args, "category");
    if (!cat_str.empty()) {
        cat_filter = category_from_string(cat_str);
    }

    uint32_t depth = 0;
    if (args.contains("depth") && args["depth"].is_number_unsigned()) {
        depth = args["depth"].get<uint32_t>();
    }

    auto entries = memory_->recall(query, limit, cat_filter);
    if (entries.empty()) {
        return ToolResult{true, "No matching memories found."};
    }

    // Follow links if depth > 0
    std::vector<MemoryEntry> neighbor_entries;
    if (depth > 0) {
        neighbor_entries = collect_neighbors(memory_, entries, limit);
    }

    std::ostringstream ss;
    ss << "Found " << entries.size() << " memories";
    if (!neighbor_entries.empty()) {
        ss << " (+" << neighbor_entries.size() << " linked)";
    }
    ss << ":\n";
    for (const auto& entry : entries) {
        ss << "- [" << category_to_string(entry.category) << "] "
           << entry.key << ": " << entry.content
           << " (score: " << entry.score << ")";
        if (!entry.links.empty()) {
            ss << " [links: ";
            for (size_t i = 0; i < entry.links.size(); i++) {
                if (i > 0) ss << ", ";
                ss << entry.links[i];
            }
            ss << "]";
        }
        ss << "\n";
    }
    for (const auto& entry : neighbor_entries) {
        ss << "- [" << category_to_string(entry.category) << "] "
           << entry.key << ": " << entry.content << " (linked)\n";
    }
    return ToolResult{true, ss.str()};
}

std::string MemoryRecallTool::description() const {
    return "Search and recall stored memories by query";
}

std::string MemoryRecallTool::parameters_json() const {
    return R"json({"type":"object","properties":{"query":{"type":"string","description":"Search query to find relevant memories"},"limit":{"type":"integer","description":"Maximum number of results (default: 5)"},"category":{"type":"string","enum":["core","knowledge","conversation"],"description":"Optional category filter"},"depth":{"type":"integer","description":"Link traversal depth: 0=flat search, 1=follow links (default: 0)"}},"required":["query"]})json";
}

} // namespace ptrclaw
