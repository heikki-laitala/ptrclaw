#pragma once
#include "../memory.hpp"
#include <nlohmann/json.hpp>

namespace ptrclaw {

// Shared JSON ↔ MemoryEntry conversion used by both JsonMemory and SqliteMemory.

inline MemoryEntry entry_from_json(const nlohmann::json& item) {
    MemoryEntry entry;
    entry.id = item.value("id", "");
    entry.key = item.value("key", "");
    entry.content = item.value("content", "");
    entry.category = category_from_string(item.value("category", "knowledge"));
    entry.timestamp = item.value("timestamp", uint64_t{0});
    entry.session_id = item.value("session_id", "");
    if (item.contains("links") && item["links"].is_array()) {
        for (const auto& lnk : item["links"]) {
            if (lnk.is_string()) entry.links.push_back(lnk.get<std::string>());
        }
    }
    return entry;
}

inline nlohmann::json entry_to_json(const MemoryEntry& entry) {
    nlohmann::json item = {
        {"id", entry.id},
        {"key", entry.key},
        {"content", entry.content},
        {"category", category_to_string(entry.category)},
        {"timestamp", entry.timestamp},
        {"session_id", entry.session_id}
    };
    if (!entry.links.empty()) {
        item["links"] = entry.links;
    }
    return item;
}

} // namespace ptrclaw
