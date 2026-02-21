#pragma once
#include "../memory.hpp"
#include <mutex>
#include <string>

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

private:
    void load();
    void save();
    double score_entry(const MemoryEntry& entry, const std::vector<std::string>& tokens) const;

    std::string path_;
    std::vector<MemoryEntry> entries_;
    mutable std::mutex mutex_;
};

} // namespace ptrclaw
