#include "response_cache.hpp"
#include "../util.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <algorithm>

namespace ptrclaw {

ResponseCache::ResponseCache(const std::string& path, uint32_t ttl_seconds, uint32_t max_entries)
    : path_(path), ttl_seconds_(ttl_seconds), max_entries_(max_entries) {
    load();
}

uint64_t ResponseCache::compute_key(const std::string& model,
                                    const std::string& system_prompt,
                                    const std::string& user_message) const {
    constexpr uint64_t fnv_offset = 14695981039346656037ULL;
    constexpr uint64_t fnv_prime  = 1099511628211ULL;

    uint64_t hash = fnv_offset;

    for (unsigned char byte : model) {
        hash ^= byte;
        hash *= fnv_prime;
    }

    // Separator byte between fields
    hash ^= static_cast<unsigned char>('\x01');
    hash *= fnv_prime;

    for (unsigned char byte : system_prompt) {
        hash ^= byte;
        hash *= fnv_prime;
    }

    hash ^= static_cast<unsigned char>('\x01');
    hash *= fnv_prime;

    for (unsigned char byte : user_message) {
        hash ^= byte;
        hash *= fnv_prime;
    }

    return hash;
}

std::optional<std::string> ResponseCache::get(const std::string& model,
                                               const std::string& system_prompt,
                                               const std::string& user_message) {
    std::lock_guard<std::mutex> lock(mutex_);

    uint64_t key = compute_key(model, system_prompt, user_message);
    auto it = entries_.find(key);
    if (it == entries_.end()) return std::nullopt;

    uint64_t now = epoch_seconds();
    if ((now - it->second.timestamp) > ttl_seconds_) {
        entries_.erase(it);
        return std::nullopt;
    }

    it->second.last_access = now;
    return it->second.response;
}

void ResponseCache::put(const std::string& model,
                        const std::string& system_prompt,
                        const std::string& user_message,
                        const std::string& response) {
    std::lock_guard<std::mutex> lock(mutex_);

    uint64_t key = compute_key(model, system_prompt, user_message);
    uint64_t now = epoch_seconds();

    entries_[key] = CacheEntry{response, now, now};

    evict();
    save();
}

void ResponseCache::evict() {
    // Must be called with mutex_ already held.

    uint64_t now = epoch_seconds();

    // Remove TTL-expired entries first.
    for (auto it = entries_.begin(); it != entries_.end(); ) {
        if ((now - it->second.timestamp) > ttl_seconds_) {
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }

    // If still over capacity, evict by oldest last_access.
    if (entries_.size() > max_entries_) {
        // Collect keys sorted by last_access ascending.
        std::vector<std::pair<uint64_t, uint64_t>> key_access; // {last_access, key}
        key_access.reserve(entries_.size());
        for (const auto& [key, entry] : entries_) {
            key_access.emplace_back(entry.last_access, key);
        }

        std::sort(key_access.begin(), key_access.end());

        size_t to_remove = entries_.size() - max_entries_;
        for (size_t i = 0; i < to_remove; ++i) {
            entries_.erase(key_access[i].second);
        }
    }
}

uint32_t ResponseCache::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<uint32_t>(entries_.size());
}

void ResponseCache::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
    save();
}

void ResponseCache::load() {
    std::ifstream file(path_);
    if (!file.is_open()) return;

    try {
        nlohmann::json j = nlohmann::json::parse(file);
        if (!j.is_array()) return;

        entries_.clear();
        for (const auto& item : j) {
            uint64_t key        = item.value("key_hash",    uint64_t{0});
            std::string resp    = item.value("response",    std::string{});
            uint64_t ts         = item.value("timestamp",   uint64_t{0});
            uint64_t la         = item.value("last_access", uint64_t{0});

            if (key == 0) continue;
            entries_[key] = CacheEntry{std::move(resp), ts, la};
        }
    } catch (...) { // NOLINT(bugprone-empty-catch)
        // Corrupt file — start fresh.
    }
}

void ResponseCache::save() {
    // Must be called with mutex_ already held.

    nlohmann::json j = nlohmann::json::array();
    for (const auto& [key, entry] : entries_) {
        j.push_back({
            {"key_hash",    key},
            {"response",    entry.response},
            {"timestamp",   entry.timestamp},
            {"last_access", entry.last_access}
        });
    }

    atomic_write_file(path_, j.dump(2));
}

} // namespace ptrclaw
