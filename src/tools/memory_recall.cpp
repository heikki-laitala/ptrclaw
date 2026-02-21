#include "memory_recall.hpp"
#include "../plugin.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <sstream>

static ptrclaw::ToolRegistrar reg_memory_recall("memory_recall",
    []() { return std::make_unique<ptrclaw::MemoryRecallTool>(); });

namespace ptrclaw {

ToolResult MemoryRecallTool::execute(const std::string& args_json) {
    if (!memory_) {
        return ToolResult{false, "Memory system is not enabled"};
    }

    nlohmann::json args;
    try {
        args = nlohmann::json::parse(args_json);
    } catch (const std::exception& e) {
        return ToolResult{false, std::string("Failed to parse arguments: ") + e.what()};
    }

    if (!args.contains("query") || !args["query"].is_string()) {
        return ToolResult{false, "Missing required parameter: query"};
    }

    std::string query = args["query"].get<std::string>();

    uint32_t limit = 5;
    if (args.contains("limit") && args["limit"].is_number_unsigned()) {
        limit = args["limit"].get<uint32_t>();
    }

    std::optional<MemoryCategory> cat_filter;
    if (args.contains("category") && args["category"].is_string()) {
        cat_filter = category_from_string(args["category"].get<std::string>());
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
        std::vector<std::string> seen_keys;
        seen_keys.reserve(entries.size());
        for (const auto& e : entries) {
            seen_keys.push_back(e.key);
        }
        for (const auto& entry : entries) {
            if (entry.links.empty()) continue;
            auto neighbors = memory_->neighbors(entry.key, limit);
            for (auto& n : neighbors) {
                if (std::find(seen_keys.begin(), seen_keys.end(), n.key) == seen_keys.end()) {
                    seen_keys.push_back(n.key);
                    neighbor_entries.push_back(std::move(n));
                }
            }
        }
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
