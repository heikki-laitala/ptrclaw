#include "json_memory.hpp"
#include "../plugin.hpp"
#include "../util.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <algorithm>
#include <cctype>

static ptrclaw::MemoryRegistrar reg_json("json",
    [](const ptrclaw::Config& config) {
        std::string path = config.memory.path;
        if (path.empty()) {
            path = ptrclaw::expand_home("~/.ptrclaw/memory.json");
        }
        return std::make_unique<ptrclaw::JsonMemory>(path);
    });

namespace ptrclaw {

static MemoryEntry entry_from_json(const nlohmann::json& item) {
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

static nlohmann::json entry_to_json(const MemoryEntry& entry) {
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

JsonMemory::JsonMemory(const std::string& path) : path_(path) {
    load();
}

void JsonMemory::load() {
    std::ifstream file(path_);
    if (!file.is_open()) return;

    try {
        nlohmann::json j = nlohmann::json::parse(file);
        if (!j.is_array()) return;

        entries_.clear();
        entries_.reserve(j.size());
        for (const auto& item : j) {
            entries_.push_back(entry_from_json(item));
        }
    } catch (...) { // NOLINT(bugprone-empty-catch)
        // Corrupt file — start fresh
    }
}

void JsonMemory::save() {
    nlohmann::json j = nlohmann::json::array();
    for (const auto& entry : entries_) {
        j.push_back(entry_to_json(entry));
    }
    atomic_write_file(path_, j.dump(2));
}

static std::string to_lower(const std::string& s) {
    std::string result = s;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

static std::vector<std::string> tokenize(const std::string& s) {
    std::vector<std::string> tokens;
    std::string lower = to_lower(s);
    std::string token;
    for (char c : lower) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            token += c;
        } else if (!token.empty()) {
            tokens.push_back(std::move(token));
            token.clear();
        }
    }
    if (!token.empty()) {
        tokens.push_back(std::move(token));
    }
    return tokens;
}

double JsonMemory::score_entry(const MemoryEntry& entry,
                                const std::vector<std::string>& tokens) const {
    if (tokens.empty()) return 0.0;

    std::string haystack = to_lower(entry.key) + " " + to_lower(entry.content);
    double hits = 0.0;
    for (const auto& token : tokens) {
        if (haystack.find(token) != std::string::npos) {
            hits += 1.0;
        }
    }
    return hits / static_cast<double>(tokens.size());
}

std::string JsonMemory::store(const std::string& key, const std::string& content,
                               MemoryCategory category, const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Upsert: find existing entry by key
    for (auto& entry : entries_) {
        if (entry.key == key) {
            entry.content = content;
            entry.category = category;
            entry.timestamp = epoch_seconds();
            entry.session_id = session_id;
            save();
            return entry.id;
        }
    }

    // New entry
    MemoryEntry entry;
    entry.id = generate_id();
    entry.key = key;
    entry.content = content;
    entry.category = category;
    entry.timestamp = epoch_seconds();
    entry.session_id = session_id;
    entries_.push_back(std::move(entry));
    save();
    return entries_.back().id;
}

std::vector<MemoryEntry> JsonMemory::recall(const std::string& query, uint32_t limit,
                                             std::optional<MemoryCategory> category_filter) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto tokens = tokenize(query);
    if (tokens.empty()) return {};

    std::vector<std::pair<double, size_t>> scored;
    for (size_t i = 0; i < entries_.size(); i++) {
        if (category_filter && entries_[i].category != *category_filter) continue;

        double s = score_entry(entries_[i], tokens);
        if (s > 0.0) {
            scored.emplace_back(s, i);
        }
    }

    std::sort(scored.begin(), scored.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });

    std::vector<MemoryEntry> result;
    uint32_t count = 0;
    for (const auto& [s, idx] : scored) {
        if (count >= limit) break;
        MemoryEntry entry = entries_[idx];
        entry.score = s;
        result.push_back(std::move(entry));
        count++;
    }
    return result;
}

std::optional<MemoryEntry> JsonMemory::get(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);

    for (const auto& entry : entries_) {
        if (entry.key == key) return entry;
    }
    return std::nullopt;
}

std::vector<MemoryEntry> JsonMemory::list(std::optional<MemoryCategory> category_filter,
                                           uint32_t limit) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<MemoryEntry> result;
    for (const auto& entry : entries_) {
        if (category_filter && entry.category != *category_filter) continue;
        result.push_back(entry);
        if (result.size() >= limit) break;
    }
    return result;
}

bool JsonMemory::forget(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = std::find_if(entries_.begin(), entries_.end(),
                           [&key](const MemoryEntry& e) { return e.key == key; });
    if (it == entries_.end()) return false;

    // Clean up links referencing this key in other entries
    for (auto& entry : entries_) {
        auto& lnks = entry.links;
        lnks.erase(std::remove(lnks.begin(), lnks.end(), key), lnks.end());
    }

    entries_.erase(it);
    save();
    return true;
}

uint32_t JsonMemory::count(std::optional<MemoryCategory> category_filter) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!category_filter) return static_cast<uint32_t>(entries_.size());

    uint32_t n = 0;
    for (const auto& entry : entries_) {
        if (entry.category == *category_filter) n++;
    }
    return n;
}

std::string JsonMemory::snapshot_export() {
    std::lock_guard<std::mutex> lock(mutex_);

    nlohmann::json j = nlohmann::json::array();
    for (const auto& entry : entries_) {
        j.push_back(entry_to_json(entry));
    }
    return j.dump(2);
}

uint32_t JsonMemory::snapshot_import(const std::string& json_str) {
    std::lock_guard<std::mutex> lock(mutex_);

    uint32_t imported = 0;
    try {
        nlohmann::json j = nlohmann::json::parse(json_str);
        if (!j.is_array()) return 0;

        for (const auto& item : j) {
            std::string key = item.value("key", "");
            if (key.empty()) continue;

            // Skip if key already exists
            bool exists = false;
            for (const auto& e : entries_) {
                if (e.key == key) { exists = true; break; }
            }
            if (exists) continue;

            auto entry = entry_from_json(item);
            if (entry.id.empty()) entry.id = generate_id();
            if (entry.timestamp == 0) entry.timestamp = epoch_seconds();
            entries_.push_back(std::move(entry));
            imported++;
        }

        if (imported > 0) save();
    } catch (...) { // NOLINT(bugprone-empty-catch)
    }
    return imported;
}

uint32_t JsonMemory::hygiene_purge(uint32_t max_age_seconds) {
    std::lock_guard<std::mutex> lock(mutex_);

    uint64_t cutoff = epoch_seconds() - max_age_seconds;
    uint32_t purged = 0;

    // Collect keys being purged
    std::vector<std::string> purged_keys;
    auto it = entries_.begin();
    while (it != entries_.end()) {
        if (it->category == MemoryCategory::Conversation && it->timestamp < cutoff) {
            purged_keys.push_back(it->key);
            it = entries_.erase(it);
            purged++;
        } else {
            ++it;
        }
    }

    // Clean up dangling links
    if (!purged_keys.empty()) {
        for (auto& entry : entries_) {
            auto& lnks = entry.links;
            lnks.erase(std::remove_if(lnks.begin(), lnks.end(),
                [&purged_keys](const std::string& k) {
                    return std::find(purged_keys.begin(), purged_keys.end(), k) != purged_keys.end();
                }), lnks.end());
        }
        save();
    }

    return purged;
}

bool JsonMemory::link(const std::string& from_key, const std::string& to_key) {
    std::lock_guard<std::mutex> lock(mutex_);

    MemoryEntry* from_entry = nullptr;
    MemoryEntry* to_entry = nullptr;
    for (auto& e : entries_) {
        if (e.key == from_key) from_entry = &e;
        if (e.key == to_key) to_entry = &e;
        if (from_entry && to_entry) break;
    }
    if (!from_entry || !to_entry) return false;

    // Add to_key to from_entry's links if not already present
    if (std::find(from_entry->links.begin(), from_entry->links.end(), to_key) == from_entry->links.end()) {
        from_entry->links.push_back(to_key);
    }
    // Add from_key to to_entry's links if not already present
    if (std::find(to_entry->links.begin(), to_entry->links.end(), from_key) == to_entry->links.end()) {
        to_entry->links.push_back(from_key);
    }

    save();
    return true;
}

bool JsonMemory::unlink(const std::string& from_key, const std::string& to_key) {
    std::lock_guard<std::mutex> lock(mutex_);

    MemoryEntry* from_entry = nullptr;
    MemoryEntry* to_entry = nullptr;
    for (auto& e : entries_) {
        if (e.key == from_key) from_entry = &e;
        if (e.key == to_key) to_entry = &e;
        if (from_entry && to_entry) break;
    }
    if (!from_entry || !to_entry) return false;

    auto& from_links = from_entry->links;
    auto& to_links = to_entry->links;
    auto fit = std::find(from_links.begin(), from_links.end(), to_key);
    auto tit = std::find(to_links.begin(), to_links.end(), from_key);
    if (fit == from_links.end() && tit == to_links.end()) return false;

    if (fit != from_links.end()) from_links.erase(fit);
    if (tit != to_links.end()) to_links.erase(tit);

    save();
    return true;
}

std::vector<MemoryEntry> JsonMemory::neighbors(const std::string& key, uint32_t limit) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Find the source entry
    const MemoryEntry* source = nullptr;
    for (const auto& e : entries_) {
        if (e.key == key) { source = &e; break; }
    }
    if (!source) return {};

    std::vector<MemoryEntry> result;
    for (const auto& linked_key : source->links) {
        if (result.size() >= limit) break;
        for (const auto& e : entries_) {
            if (e.key == linked_key) {
                result.push_back(e);
                break;
            }
        }
    }
    return result;
}

} // namespace ptrclaw
