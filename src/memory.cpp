#include "memory.hpp"
#include "config.hpp"
#include "plugin.hpp"
#include "util.hpp"
#include <algorithm>
#include <cstdio>
#include <filesystem>
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
                          uint32_t recall_limit, uint32_t enrich_depth) {
    if (!memory || recall_limit == 0) return user_message;

    // Over-fetch to compensate for Core entries we'll filter out (they're in the system prompt)
    auto entries = memory->recall(user_message, recall_limit * 2, std::nullopt);

    entries.erase(std::remove_if(entries.begin(), entries.end(),
        [](const MemoryEntry& e) { return e.category == MemoryCategory::Core; }),
        entries.end());
    if (entries.empty()) return user_message;
    if (entries.size() > recall_limit) entries.resize(recall_limit);

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

std::string default_memory_path(const std::string& backend) {
    if (backend == "sqlite") return expand_home("~/.ptrclaw/memory.db");
    return expand_home("~/.ptrclaw/memory.json");
}

std::string session_store_path(const std::string& base_path,
                               const std::string& session_id) {
    // Leading hash: two ids that sanitize to the same text still get distinct
    // directories, and no id can produce a component of "." or "..".
    //
    // All 16 hex digits, not a prefix. Ids longer than kMaxIdChars that share a
    // prefix sanitize to the same text, so this is the only thing separating
    // their stores — and it is the boundary the isolation mode promises. FNV-1a
    // is offline-computable, so a truncated 32-bit key would let a caller who
    // knows a victim's prefix search suffixes until it collides and be routed
    // into that session's memory and response cache.
    char hash[17];
    std::snprintf(hash, sizeof(hash), "%016llx",
                  static_cast<unsigned long long>(fnv1a(session_id)));

    std::string key(hash);
    key += '-';

    constexpr size_t kMaxIdChars = 32;
    size_t taken = 0;
    for (char c : session_id) {
        if (taken++ == kMaxIdChars) break;
        bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        key += safe ? c : '_';
    }

    std::filesystem::path base(base_path);
    std::filesystem::path dir = base.parent_path();
    std::filesystem::path name = base.filename();
    if (name.empty()) name = "memory.json";

    return (dir / "sessions" / key / name).string();
}

std::unique_ptr<Memory> create_memory(const Config& config,
                                      const std::string& session_id) {
    const auto& backend = config.memory.backend;
    auto& registry = PluginRegistry::instance();

    if (!registry.has_memory(backend)) {
        // Fall back to "none" if configured backend is missing
        if (registry.has_memory("none")) {
            return registry.create_memory("none", config);
        }
        return nullptr;
    }

    if (config.memory.isolation == "session" && !session_id.empty()) {
        // A copy so the backend factory can read memory.path as it always has,
        // rather than every backend growing a path-override parameter.
        Config scoped = config;
        std::string base = config.memory.path.empty()
            ? default_memory_path(backend)
            : expand_home(config.memory.path);
        scoped.memory.path = session_store_path(base, session_id);
        return registry.create_memory(backend, scoped);
    }

    return registry.create_memory(backend, config);
}

} // namespace ptrclaw
