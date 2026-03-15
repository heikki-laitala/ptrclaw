#include "memory.hpp"
#include "config.hpp"
#include "plugin.hpp"
#include <algorithm>
#include <sstream>
#include <unordered_set>

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
    if (!memory) return {};

    // Track visited keys to prevent cycles and dedup
    std::unordered_set<std::string> seen_keys;
    seen_keys.reserve(entries.size());
    for (const auto& e : entries) {
        seen_keys.insert(e.key);
    }

    std::vector<MemoryEntry> result;
    for (const auto& entry : entries) {
        if (entry.links.empty()) continue;
        auto neighbors = memory->neighbors(entry.key, limit);
        for (auto& n : neighbors) {
            if (seen_keys.insert(n.key).second) {
                result.push_back(std::move(n));
            }
        }
    }
    return result;
}

std::string memory_enrich(Memory* memory, const std::string& user_message,
                          uint32_t recall_limit, uint32_t enrich_depth,
                          const std::string& episode_context) {
    // Collect recalled entries when memory is available
    std::vector<MemoryEntry> entries;
    if (memory && recall_limit > 0) {
        // Over-fetch to compensate for Core entries we'll filter out (they're in the system prompt)
        entries = memory->recall(user_message, recall_limit * 2, std::nullopt);
        entries.erase(std::remove_if(entries.begin(), entries.end(),
            [](const MemoryEntry& e) { return e.category == MemoryCategory::Core; }),
            entries.end());
        if (entries.size() > recall_limit) entries.resize(recall_limit);
    }

    std::vector<MemoryEntry> neighbor_entries;
    if (enrich_depth > 0 && memory && !entries.empty()) {
        neighbor_entries = collect_neighbors(memory, entries, recall_limit);
    }

    // Split into concepts (cross-session, session_id empty) and
    // observations (session-specific, session_id set). Neighbors follow the same rule.
    std::vector<const MemoryEntry*> concepts, observations;
    for (const auto& e : entries) {
        (e.session_id.empty() ? concepts : observations).push_back(&e);
    }
    for (const auto& e : neighbor_entries) {
        (e.session_id.empty() ? concepts : observations).push_back(&e);
    }

    bool has_content = !concepts.empty() || !observations.empty() || !episode_context.empty();
    if (!has_content) return user_message;

    auto format_links = [](std::ostringstream& out, const MemoryEntry* e) {
        if (!e->links.empty()) {
            out << " [links: ";
            for (size_t i = 0; i < e->links.size(); i++) {
                if (i > 0) out << ", ";
                out << e->links[i];
            }
            out << "]";
        }
    };

    std::ostringstream ss;
    ss << "[Memory context]\n";

    if (!concepts.empty()) {
        ss << "Concepts:\n";
        for (const auto* e : concepts) {
            ss << "- " << e->key << ": " << e->content;
            format_links(ss, e);
            ss << "\n";
        }
    }

    if (!observations.empty()) {
        ss << "Observations:\n";
        for (const auto* e : observations) {
            ss << "- " << e->key << ": " << e->content;
            format_links(ss, e);
            ss << "\n";
        }
    }

    if (!episode_context.empty()) {
        ss << episode_context << "\n";
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
