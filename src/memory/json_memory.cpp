#include "json_memory.hpp"
#include "entry_json.hpp"
#include "../plugin.hpp"
#include "../util.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <algorithm>

static ptrclaw::MemoryRegistrar reg_json("json",
    [](const ptrclaw::Config& config) {
        std::string path = config.memory.path;
        if (path.empty()) {
            path = ptrclaw::expand_home("~/.ptrclaw/memory.json");
        }
        return std::make_unique<ptrclaw::JsonMemory>(path);
    });

namespace ptrclaw {

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
        rebuild_index();
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

void JsonMemory::remove_links_to(const std::vector<std::string>& dead_keys) {
    for (auto& entry : entries_) {
        auto& lnks = entry.links;
        lnks.erase(std::remove_if(lnks.begin(), lnks.end(),
            [&dead_keys](const std::string& k) {
                return std::find(dead_keys.begin(), dead_keys.end(), k) != dead_keys.end();
            }), lnks.end());
    }
}

void JsonMemory::rebuild_index() {
    key_index_.clear();
    key_index_.reserve(entries_.size());
    for (size_t i = 0; i < entries_.size(); ++i) {
        key_index_[entries_[i].key] = i;
    }
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

    // Word-boundary matching with 2x key weight.
    // Tokenizing both sides prevents substring false positives (e.g. "test" matching "attest").
    auto key_tokens = tokenize(entry.key);
    auto content_tokens = tokenize(entry.content);

    double score = 0.0;
    for (const auto& token : tokens) {
        bool in_key = std::find(key_tokens.begin(), key_tokens.end(), token) != key_tokens.end();
        bool in_content = std::find(content_tokens.begin(), content_tokens.end(), token) != content_tokens.end();

        if (in_key) {
            score += 2.0;  // key matches weighted 2x
        } else if (in_content) {
            score += 1.0;
        }
    }
    return score / static_cast<double>(tokens.size());
}

std::string JsonMemory::store(const std::string& key, const std::string& content,
                               MemoryCategory category, const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Upsert: O(1) lookup via key index
    auto it = key_index_.find(key);
    if (it != key_index_.end()) {
        auto& entry = entries_[it->second];
        entry.content = content;
        entry.category = category;
        entry.timestamp = epoch_seconds();
        entry.session_id = session_id;
        save();
        return entry.id;
    }

    // New entry
    MemoryEntry entry;
    entry.id = generate_id();
    entry.key = key;
    entry.content = content;
    entry.category = category;
    entry.timestamp = epoch_seconds();
    entry.session_id = session_id;
    key_index_[key] = entries_.size();
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

    // partial_sort: only sort the top-K elements instead of the full vector
    size_t k = std::min(static_cast<size_t>(limit), scored.size());
    std::partial_sort(scored.begin(), scored.begin() + static_cast<ptrdiff_t>(k), scored.end(),
                      [](const auto& a, const auto& b) { return a.first > b.first; });

    std::vector<MemoryEntry> result;
    for (size_t i = 0; i < k; i++) {
        MemoryEntry entry = entries_[scored[i].second];
        entry.score = scored[i].first;
        result.push_back(std::move(entry));
    }
    return result;
}

std::optional<MemoryEntry> JsonMemory::get(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = key_index_.find(key);
    if (it != key_index_.end()) return entries_[it->second];
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

    auto idx_it = key_index_.find(key);
    if (idx_it == key_index_.end()) return false;

    remove_links_to({key});
    entries_.erase(entries_.begin() + static_cast<ptrdiff_t>(idx_it->second));
    rebuild_index();
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

            // Skip if key already exists (O(1) via index)
            if (key_index_.count(key)) continue;

            auto entry = entry_from_json(item);
            if (entry.id.empty()) entry.id = generate_id();
            if (entry.timestamp == 0) entry.timestamp = epoch_seconds();
            key_index_[key] = entries_.size();
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
        if (it->category == MemoryCategory::Conversation && it->timestamp <= cutoff) {
            purged_keys.push_back(it->key);
            it = entries_.erase(it);
            purged++;
        } else {
            ++it;
        }
    }

    if (!purged_keys.empty()) {
        remove_links_to(purged_keys);
        rebuild_index();
        save();
    }

    return purged;
}

bool JsonMemory::link(const std::string& from_key, const std::string& to_key) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto from_it = key_index_.find(from_key);
    auto to_it = key_index_.find(to_key);
    if (from_it == key_index_.end() || to_it == key_index_.end()) return false;

    auto& from_entry = entries_[from_it->second];
    auto& to_entry = entries_[to_it->second];

    // Add to_key to from_entry's links if not already present
    if (std::find(from_entry.links.begin(), from_entry.links.end(), to_key) == from_entry.links.end()) {
        from_entry.links.push_back(to_key);
    }
    // Add from_key to to_entry's links if not already present
    if (std::find(to_entry.links.begin(), to_entry.links.end(), from_key) == to_entry.links.end()) {
        to_entry.links.push_back(from_key);
    }

    save();
    return true;
}

bool JsonMemory::unlink(const std::string& from_key, const std::string& to_key) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto from_it = key_index_.find(from_key);
    auto to_it = key_index_.find(to_key);
    if (from_it == key_index_.end() || to_it == key_index_.end()) return false;

    auto& from_links = entries_[from_it->second].links;
    auto& to_links = entries_[to_it->second].links;
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

    // Find the source entry via index
    auto src_it = key_index_.find(key);
    if (src_it == key_index_.end()) return {};

    const auto& source = entries_[src_it->second];
    std::vector<MemoryEntry> result;
    for (const auto& linked_key : source.links) {
        if (result.size() >= limit) break;
        auto lnk_it = key_index_.find(linked_key);
        if (lnk_it != key_index_.end()) {
            result.push_back(entries_[lnk_it->second]);
        }
    }
    return result;
}

} // namespace ptrclaw
