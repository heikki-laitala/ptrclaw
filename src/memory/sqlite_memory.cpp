#include "sqlite_memory.hpp"
#include "../plugin.hpp"
#include "../util.hpp"
#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include <filesystem>
#include <stdexcept>

static ptrclaw::MemoryRegistrar reg_sqlite("sqlite",
    [](const ptrclaw::Config& config) {
        std::string path = config.memory.path;
        if (path.empty()) {
            path = ptrclaw::expand_home("~/.ptrclaw/memory.db");
        }
        return std::make_unique<ptrclaw::SqliteMemory>(path);
    });

namespace ptrclaw {

// RAII wrapper for sqlite3_stmt
struct StmtGuard {
    sqlite3_stmt* stmt = nullptr;
    ~StmtGuard() { if (stmt) sqlite3_finalize(stmt); }
};

SqliteMemory::SqliteMemory(const std::string& path) : path_(path) {
    // Ensure parent directory exists
    auto parent = std::filesystem::path(path_).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }

    if (sqlite3_open(path_.c_str(), &db_) != SQLITE_OK) {
        std::string err = db_ ? sqlite3_errmsg(db_) : "unknown error";
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        throw std::runtime_error("SqliteMemory: failed to open database: " + err);
    }

    // Performance pragmas
    sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA temp_store=MEMORY;", nullptr, nullptr, nullptr);
    // Allow FTS5 virtual table use inside triggers (required since SQLite 3.37)
    sqlite3_exec(db_, "PRAGMA trusted_schema=ON;", nullptr, nullptr, nullptr);

    init_schema();
}

SqliteMemory::~SqliteMemory() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

void SqliteMemory::init_schema() {
    // Main memories table
    const char* create_table =
        "CREATE TABLE IF NOT EXISTS memories ("
        "  id         TEXT PRIMARY KEY,"
        "  key        TEXT UNIQUE NOT NULL,"
        "  content    TEXT NOT NULL,"
        "  category   TEXT NOT NULL,"
        "  timestamp  INTEGER NOT NULL,"
        "  session_id TEXT NOT NULL"
        ");";
    sqlite3_exec(db_, create_table, nullptr, nullptr, nullptr);

    // FTS5 virtual table (content table referencing memories)
    const char* create_fts =
        "CREATE VIRTUAL TABLE IF NOT EXISTS memories_fts "
        "USING fts5(key, content, content=memories, content_rowid=rowid);";
    sqlite3_exec(db_, create_fts, nullptr, nullptr, nullptr);

    // Triggers to keep FTS in sync with the memories table

    // After insert: add new row to FTS
    const char* trigger_insert =
        "CREATE TRIGGER IF NOT EXISTS memories_ai AFTER INSERT ON memories BEGIN"
        "  INSERT INTO memories_fts(rowid, key, content)"
        "  VALUES (new.rowid, new.key, new.content);"
        "END;";
    sqlite3_exec(db_, trigger_insert, nullptr, nullptr, nullptr);

    // After delete: remove old row from FTS
    const char* trigger_delete =
        "CREATE TRIGGER IF NOT EXISTS memories_ad AFTER DELETE ON memories BEGIN"
        "  INSERT INTO memories_fts(memories_fts, rowid, key, content)"
        "  VALUES ('delete', old.rowid, old.key, old.content);"
        "END;";
    sqlite3_exec(db_, trigger_delete, nullptr, nullptr, nullptr);

    // After update: update FTS (delete old, insert new)
    const char* trigger_update =
        "CREATE TRIGGER IF NOT EXISTS memories_au AFTER UPDATE ON memories BEGIN"
        "  INSERT INTO memories_fts(memories_fts, rowid, key, content)"
        "  VALUES ('delete', old.rowid, old.key, old.content);"
        "  INSERT INTO memories_fts(rowid, key, content)"
        "  VALUES (new.rowid, new.key, new.content);"
        "END;";
    sqlite3_exec(db_, trigger_update, nullptr, nullptr, nullptr);
}

// Helper: read a full MemoryEntry from a prepared statement that has selected
// id, key, content, category, timestamp, session_id (columns 0-5).
static MemoryEntry entry_from_stmt(sqlite3_stmt* stmt) {
    MemoryEntry entry;
    if (auto* v = sqlite3_column_text(stmt, 0)) entry.id         = reinterpret_cast<const char*>(v);
    if (auto* v = sqlite3_column_text(stmt, 1)) entry.key        = reinterpret_cast<const char*>(v);
    if (auto* v = sqlite3_column_text(stmt, 2)) entry.content    = reinterpret_cast<const char*>(v);
    if (auto* v = sqlite3_column_text(stmt, 3)) entry.category   = category_from_string(reinterpret_cast<const char*>(v));
    entry.timestamp  = static_cast<uint64_t>(sqlite3_column_int64(stmt, 4));
    if (auto* v = sqlite3_column_text(stmt, 5)) entry.session_id = reinterpret_cast<const char*>(v);
    return entry;
}

std::string SqliteMemory::store(const std::string& key, const std::string& content,
                                 MemoryCategory category, const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Check if key already exists to reuse its id
    std::string existing_id;
    {
        StmtGuard g;
        const char* sql = "SELECT id FROM memories WHERE key = ?;";
        if (sqlite3_prepare_v2(db_, sql, -1, &g.stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(g.stmt, 1, key.c_str(), -1, SQLITE_STATIC);
            if (sqlite3_step(g.stmt) == SQLITE_ROW) {
                if (auto* v = sqlite3_column_text(g.stmt, 0)) {
                    existing_id = reinterpret_cast<const char*>(v);
                }
            }
        }
    }

    std::string id = existing_id.empty() ? generate_id() : existing_id;
    auto ts = static_cast<int64_t>(epoch_seconds());
    std::string cat = category_to_string(category);

    StmtGuard g;
    const char* sql =
        "INSERT OR REPLACE INTO memories (id, key, content, category, timestamp, session_id)"
        " VALUES (?, ?, ?, ?, ?, ?);";
    if (sqlite3_prepare_v2(db_, sql, -1, &g.stmt, nullptr) != SQLITE_OK) {
        return id;
    }
    sqlite3_bind_text(g.stmt, 1, id.c_str(),         -1, SQLITE_STATIC);
    sqlite3_bind_text(g.stmt, 2, key.c_str(),        -1, SQLITE_STATIC);
    sqlite3_bind_text(g.stmt, 3, content.c_str(),    -1, SQLITE_STATIC);
    sqlite3_bind_text(g.stmt, 4, cat.c_str(),        -1, SQLITE_STATIC);
    sqlite3_bind_int64(g.stmt, 5, ts);
    sqlite3_bind_text(g.stmt, 6, session_id.c_str(), -1, SQLITE_STATIC);
    sqlite3_step(g.stmt);

    return id;
}

std::vector<MemoryEntry> SqliteMemory::recall(const std::string& query, uint32_t limit,
                                               std::optional<MemoryCategory> category_filter) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<MemoryEntry> results;

    // Try FTS5 MATCH first
    bool fts_ok = false;
    {
        std::string fts_sql;
        if (category_filter) {
            fts_sql =
                "SELECT m.id, m.key, m.content, m.category, m.timestamp, m.session_id,"
                "       bm25(memories_fts) AS score"
                " FROM memories_fts"
                " JOIN memories AS m ON memories_fts.rowid = m.rowid"
                " WHERE memories_fts MATCH ?"
                "   AND m.category = ?"
                " ORDER BY bm25(memories_fts)"
                " LIMIT ?;";
        } else {
            fts_sql =
                "SELECT m.id, m.key, m.content, m.category, m.timestamp, m.session_id,"
                "       bm25(memories_fts) AS score"
                " FROM memories_fts"
                " JOIN memories AS m ON memories_fts.rowid = m.rowid"
                " WHERE memories_fts MATCH ?"
                " ORDER BY bm25(memories_fts)"
                " LIMIT ?;";
        }

        StmtGuard g;
        if (sqlite3_prepare_v2(db_, fts_sql.c_str(), -1, &g.stmt, nullptr) == SQLITE_OK) {
            int col = 1;
            sqlite3_bind_text(g.stmt, col++, query.c_str(), -1, SQLITE_STATIC);
            if (category_filter) {
                std::string cat = category_to_string(*category_filter);
                sqlite3_bind_text(g.stmt, col++, cat.c_str(), -1, SQLITE_TRANSIENT);
            }
            sqlite3_bind_int(g.stmt, col, static_cast<int>(limit));

            int rc = sqlite3_step(g.stmt);
            if (rc == SQLITE_ROW || rc == SQLITE_DONE) {
                fts_ok = true;
                while (rc == SQLITE_ROW) {
                    MemoryEntry entry = entry_from_stmt(g.stmt);
                    // bm25 returns negative values; negate for a positive score
                    entry.score = -sqlite3_column_double(g.stmt, 6);
                    results.push_back(std::move(entry));
                    rc = sqlite3_step(g.stmt);
                }
            }
        }
    }

    if (!fts_ok || results.empty()) {
        results.clear();

        // Fall back to LIKE search on key and content
        std::string like_pat = "%" + query + "%";
        std::string like_sql;
        if (category_filter) {
            like_sql =
                "SELECT id, key, content, category, timestamp, session_id"
                " FROM memories"
                " WHERE (key LIKE ? OR content LIKE ?)"
                "   AND category = ?"
                " ORDER BY timestamp DESC"
                " LIMIT ?;";
        } else {
            like_sql =
                "SELECT id, key, content, category, timestamp, session_id"
                " FROM memories"
                " WHERE key LIKE ? OR content LIKE ?"
                " ORDER BY timestamp DESC"
                " LIMIT ?;";
        }

        StmtGuard g;
        if (sqlite3_prepare_v2(db_, like_sql.c_str(), -1, &g.stmt, nullptr) == SQLITE_OK) {
            int col = 1;
            sqlite3_bind_text(g.stmt, col++, like_pat.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(g.stmt, col++, like_pat.c_str(), -1, SQLITE_TRANSIENT);
            if (category_filter) {
                std::string cat = category_to_string(*category_filter);
                sqlite3_bind_text(g.stmt, col++, cat.c_str(), -1, SQLITE_TRANSIENT);
            }
            sqlite3_bind_int(g.stmt, col, static_cast<int>(limit));

            int rc = sqlite3_step(g.stmt);
            while (rc == SQLITE_ROW) {
                MemoryEntry entry = entry_from_stmt(g.stmt);
                entry.score = 1.0;
                results.push_back(std::move(entry));
                rc = sqlite3_step(g.stmt);
            }
        }
    }

    return results;
}

std::optional<MemoryEntry> SqliteMemory::get(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);

    StmtGuard g;
    const char* sql =
        "SELECT id, key, content, category, timestamp, session_id"
        " FROM memories WHERE key = ?;";
    if (sqlite3_prepare_v2(db_, sql, -1, &g.stmt, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_text(g.stmt, 1, key.c_str(), -1, SQLITE_STATIC);

    if (sqlite3_step(g.stmt) == SQLITE_ROW) {
        return entry_from_stmt(g.stmt);
    }
    return std::nullopt;
}

std::vector<MemoryEntry> SqliteMemory::list(std::optional<MemoryCategory> category_filter,
                                             uint32_t limit) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string sql;
    if (category_filter) {
        sql =
            "SELECT id, key, content, category, timestamp, session_id"
            " FROM memories WHERE category = ?"
            " ORDER BY timestamp DESC LIMIT ?;";
    } else {
        sql =
            "SELECT id, key, content, category, timestamp, session_id"
            " FROM memories ORDER BY timestamp DESC LIMIT ?;";
    }

    StmtGuard g;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &g.stmt, nullptr) != SQLITE_OK) {
        return {};
    }

    int col = 1;
    if (category_filter) {
        std::string cat = category_to_string(*category_filter);
        sqlite3_bind_text(g.stmt, col++, cat.c_str(), -1, SQLITE_TRANSIENT);
    }
    sqlite3_bind_int(g.stmt, col, static_cast<int>(limit));

    std::vector<MemoryEntry> results;
    int rc = sqlite3_step(g.stmt);
    while (rc == SQLITE_ROW) {
        results.push_back(entry_from_stmt(g.stmt));
        rc = sqlite3_step(g.stmt);
    }
    return results;
}

bool SqliteMemory::forget(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);

    StmtGuard g;
    const char* sql = "DELETE FROM memories WHERE key = ?;";
    if (sqlite3_prepare_v2(db_, sql, -1, &g.stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(g.stmt, 1, key.c_str(), -1, SQLITE_STATIC);
    sqlite3_step(g.stmt);

    return sqlite3_changes(db_) > 0;
}

uint32_t SqliteMemory::count(std::optional<MemoryCategory> category_filter) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string sql;
    if (category_filter) {
        sql = "SELECT COUNT(*) FROM memories WHERE category = ?;";
    } else {
        sql = "SELECT COUNT(*) FROM memories;";
    }

    StmtGuard g;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &g.stmt, nullptr) != SQLITE_OK) {
        return 0;
    }
    if (category_filter) {
        std::string cat = category_to_string(*category_filter);
        sqlite3_bind_text(g.stmt, 1, cat.c_str(), -1, SQLITE_TRANSIENT);
    }

    if (sqlite3_step(g.stmt) == SQLITE_ROW) {
        return static_cast<uint32_t>(sqlite3_column_int(g.stmt, 0));
    }
    return 0;
}

std::string SqliteMemory::snapshot_export() {
    std::lock_guard<std::mutex> lock(mutex_);

    StmtGuard g;
    const char* sql =
        "SELECT id, key, content, category, timestamp, session_id"
        " FROM memories ORDER BY timestamp ASC;";
    if (sqlite3_prepare_v2(db_, sql, -1, &g.stmt, nullptr) != SQLITE_OK) {
        return "[]";
    }

    nlohmann::json arr = nlohmann::json::array();
    int rc = sqlite3_step(g.stmt);
    while (rc == SQLITE_ROW) {
        MemoryEntry entry = entry_from_stmt(g.stmt);
        arr.push_back({
            {"id",         entry.id},
            {"key",        entry.key},
            {"content",    entry.content},
            {"category",   category_to_string(entry.category)},
            {"timestamp",  entry.timestamp},
            {"session_id", entry.session_id}
        });
        rc = sqlite3_step(g.stmt);
    }
    return arr.dump(2);
}

uint32_t SqliteMemory::snapshot_import(const std::string& json_str) {
    std::lock_guard<std::mutex> lock(mutex_);

    uint32_t imported = 0;
    try {
        nlohmann::json j = nlohmann::json::parse(json_str);
        if (!j.is_array()) return 0;

        const char* sql =
            "INSERT OR IGNORE INTO memories (id, key, content, category, timestamp, session_id)"
            " VALUES (?, ?, ?, ?, ?, ?);";

        for (const auto& item : j) {
            std::string key = item.value("key", "");
            if (key.empty()) continue;

            std::string id         = item.value("id", generate_id());
            std::string content    = item.value("content", "");
            std::string category   = item.value("category", "knowledge");
            auto        timestamp  = static_cast<int64_t>(item.value("timestamp", epoch_seconds()));
            std::string session_id = item.value("session_id", "");

            StmtGuard g;
            if (sqlite3_prepare_v2(db_, sql, -1, &g.stmt, nullptr) != SQLITE_OK) continue;

            sqlite3_bind_text(g.stmt, 1, id.c_str(),         -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(g.stmt, 2, key.c_str(),        -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(g.stmt, 3, content.c_str(),    -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(g.stmt, 4, category.c_str(),   -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(g.stmt, 5, timestamp);
            sqlite3_bind_text(g.stmt, 6, session_id.c_str(), -1, SQLITE_TRANSIENT);

            if (sqlite3_step(g.stmt) == SQLITE_DONE && sqlite3_changes(db_) > 0) {
                imported++;
            }
        }
    } catch (...) { // NOLINT(bugprone-empty-catch)
    }
    return imported;
}

uint32_t SqliteMemory::hygiene_purge(uint32_t max_age_seconds) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto cutoff = static_cast<int64_t>(epoch_seconds()) - static_cast<int64_t>(max_age_seconds);

    StmtGuard g;
    const char* sql =
        "DELETE FROM memories WHERE category = 'conversation' AND timestamp < ?;";
    if (sqlite3_prepare_v2(db_, sql, -1, &g.stmt, nullptr) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_int64(g.stmt, 1, cutoff);
    sqlite3_step(g.stmt);

    return static_cast<uint32_t>(sqlite3_changes(db_));
}

} // namespace ptrclaw
