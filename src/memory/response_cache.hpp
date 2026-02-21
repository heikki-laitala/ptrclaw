#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <cstdint>
#include <optional>

namespace ptrclaw {

struct CacheEntry {
    std::string response;
    uint64_t timestamp;
    uint64_t last_access;
};

class ResponseCache {
public:
    ResponseCache(const std::string& path, uint32_t ttl_seconds, uint32_t max_entries);

    // Look up cached response. Returns nullopt on miss.
    std::optional<std::string> get(const std::string& model,
                                   const std::string& system_prompt,
                                   const std::string& user_message);

    // Store a response in the cache.
    void put(const std::string& model,
             const std::string& system_prompt,
             const std::string& user_message,
             const std::string& response);

    uint32_t size() const;
    void clear();

private:
    uint64_t compute_key(const std::string& model,
                         const std::string& system_prompt,
                         const std::string& user_message) const;
    void evict();
    void load();
    void save();

    std::string path_;
    uint32_t ttl_seconds_;
    uint32_t max_entries_;
    std::unordered_map<uint64_t, CacheEntry> entries_;
    mutable std::mutex mutex_;
};

} // namespace ptrclaw
