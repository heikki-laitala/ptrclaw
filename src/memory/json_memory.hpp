#pragma once
#include "../memory.hpp"
#include <mutex>
#include <string>
#include <unordered_map>

namespace ptrclaw {

class JsonMemory : public Memory {
public:
    explicit JsonMemory(const std::string& path);

    std::string backend_name() const override { return "json"; }

    std::string store(const std::string& key, const std::string& content,
                      MemoryCategory category, const std::string& session_id) override;

    std::vector<MemoryEntry> recall(const std::string& query, uint32_t limit,
                                    std::optional<MemoryCategory> category_filter) override;

    std::optional<MemoryEntry> get(const std::string& key) override;

    std::vector<MemoryEntry> list(std::optional<MemoryCategory> category_filter,
                                  uint32_t limit) override;

    bool forget(const std::string& key) override;

    uint32_t count(std::optional<MemoryCategory> category_filter) override;

    std::string snapshot_export() override;

    uint32_t snapshot_import(const std::string& json_str) override;

    uint32_t hygiene_purge(uint32_t max_age_seconds) override;

    bool link(const std::string& from_key, const std::string& to_key) override;
    bool unlink(const std::string& from_key, const std::string& to_key) override;
    std::vector<MemoryEntry> neighbors(const std::string& key, uint32_t limit) override;

private:
    void load();
    void save();
    void rebuild_index();
    void remove_links_to(const std::vector<std::string>& dead_keys);
    double score_entry(const MemoryEntry& entry, const std::vector<std::string>& tokens) const;

    std::string path_;
    std::vector<MemoryEntry> entries_;
    std::unordered_map<std::string, size_t> key_index_; // key -> entries_ index
    mutable std::mutex mutex_;
};

} // namespace ptrclaw
