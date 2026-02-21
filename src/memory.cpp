#include "memory.hpp"
#include "config.hpp"
#include "plugin.hpp"
#include <algorithm>
#include <sstream>

namespace ptrclaw {

std::string category_to_string(MemoryCategory cat) {
    switch (cat) {
        case MemoryCategory::Core:         return "core";
        case MemoryCategory::Knowledge:    return "knowledge";
        case MemoryCategory::Conversation: return "conversation";
    }
    return "knowledge";
}

MemoryCategory category_from_string(const std::string& s) {
    if (s == "core")         return MemoryCategory::Core;
    if (s == "conversation") return MemoryCategory::Conversation;
    return MemoryCategory::Knowledge;
}

std::vector<MemoryEntry> collect_neighbors(Memory* memory,
                                            const std::vector<MemoryEntry>& entries,
                                            uint32_t limit) {
    std::vector<std::string> seen_keys;
    seen_keys.reserve(entries.size());
    for (const auto& e : entries) {
        seen_keys.push_back(e.key);
    }

    std::vector<MemoryEntry> result;
    for (const auto& entry : entries) {
        if (entry.links.empty()) continue;
        auto neighbors = memory->neighbors(entry.key, limit);
        for (auto& n : neighbors) {
            if (std::find(seen_keys.begin(), seen_keys.end(), n.key) == seen_keys.end()) {
                seen_keys.push_back(n.key);
                result.push_back(std::move(n));
            }
        }
    }
    return result;
}

std::string memory_enrich(Memory* memory, const std::string& user_message,
                          uint32_t recall_limit, uint32_t enrich_depth) {
    if (!memory || recall_limit == 0) return user_message;

    auto entries = memory->recall(user_message, recall_limit, std::nullopt);
    if (entries.empty()) return user_message;

    std::vector<MemoryEntry> neighbor_entries;
    if (enrich_depth > 0) {
        neighbor_entries = collect_neighbors(memory, entries, recall_limit);
    }

    std::ostringstream ss;
    ss << "[Memory context]\n";
    for (const auto& entry : entries) {
        ss << "- " << entry.key << ": " << entry.content;
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
        ss << "- " << entry.key << ": " << entry.content << "\n";
    }
    ss << "[/Memory context]\n\n" << user_message;
    return ss.str();
}

std::unique_ptr<Memory> create_memory(const Config& config) {
    const auto& backend = config.memory.backend;
    auto& registry = PluginRegistry::instance();

    if (!registry.has_memory(backend)) {
        // Fall back to "none" if configured backend is missing
        if (registry.has_memory("none")) {
            return registry.create_memory("none", config);
        }
        return nullptr;
    }

    return registry.create_memory(backend, config);
}

} // namespace ptrclaw
