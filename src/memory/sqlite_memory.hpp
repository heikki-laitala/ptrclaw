#pragma once
#include "../memory.hpp"
#include <mutex>
#include <string>

struct sqlite3; // forward declare

namespace ptrclaw {

class SqliteMemory : public Memory {
public:
    explicit SqliteMemory(const std::string& path);
    ~SqliteMemory() override;

    // Non-copyable
    SqliteMemory(const SqliteMemory&) = delete;
    SqliteMemory& operator=(const SqliteMemory&) = delete;

    std::string backend_name() const override { return "sqlite"; }

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
    void init_schema();
    void populate_links(MemoryEntry& entry);

    sqlite3* db_ = nullptr;
    std::string path_;
    mutable std::mutex mutex_;
};

} // namespace ptrclaw
