#include "sqlite_memory.hpp"
#include "entry_json.hpp"
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
        return std::make_unique<ptrclaw::SqliteMemory>(path, config.memory.sqlite_trusted_schema);
    });

namespace ptrclaw {

// Preprocess a user query for FTS5: split on non-alphanumeric, skip
// single-char tokens, and OR-join the remainder so that any matching
// token produces results (FTS5 defaults to implicit AND).
static std::string build_fts_query(const std::string& query) {
    std::string result;
    std::string token;
    for (char c : query) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            token += c;
        } else {
            if (token.size() >= 2) {
                if (!result.empty()) result += " OR ";
                result += token;
            }
            token.clear();
        }
    }
    if (token.size() >= 2) {
        if (!result.empty()) result += " OR ";
        result += token;
    }
    return result;
}

static void exec_or_throw(sqlite3* db, const char* sql, const char* context) {
    char* err = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::string msg = context;
        msg += ": ";
        msg += err ? err : "sqlite error";
        if (err) sqlite3_free(err);
        throw std::runtime_error(msg);
    }
}

// RAII wrapper for sqlite3_stmt
struct StmtGuard {
    sqlite3_stmt* stmt = nullptr;
    ~StmtGuard() { if (stmt) sqlite3_finalize(stmt); }
};

SqliteMemory::SqliteMemory(const std::string& path, bool trusted_schema)
    : path_(path), trusted_schema_(trusted_schema) {
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
    exec_or_throw(db_, "PRAGMA journal_mode=WAL;", "SqliteMemory pragma journal_mode=WAL");
    exec_or_throw(db_, "PRAGMA synchronous=NORMAL;", "SqliteMemory pragma synchronous=NORMAL");
    exec_or_throw(db_, "PRAGMA temp_store=MEMORY;", "SqliteMemory pragma temp_store=MEMORY");

    // Security posture: keep trusted_schema OFF by default.
    // Enable only via config when legacy trigger behavior requires it.
    exec_or_throw(db_, trusted_schema_ ? "PRAGMA trusted_schema=ON;" : "PRAGMA trusted_schema=OFF;",
                  "SqliteMemory pragma trusted_schema");

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
    exec_or_throw(db_, create_table, "SqliteMemory create memories table");

    // FTS5 virtual table (content table referencing memories)
    const char* create_fts =
        "CREATE VIRTUAL TABLE IF NOT EXISTS memories_fts "
        "USING fts5(key, content, content=memories, content_rowid=rowid);";
    exec_or_throw(db_, create_fts, "SqliteMemory create memories_fts");

    // Triggers to keep FTS in sync with the memories table

    // After insert: add new row to FTS
    const char* trigger_insert =
        "CREATE TRIGGER IF NOT EXISTS memories_ai AFTER INSERT ON memories BEGIN"
        "  INSERT INTO memories_fts(rowid, key, content)"
        "  VALUES (new.rowid, new.key, new.content);"
        "END;";
    exec_or_throw(db_, trigger_insert, "SqliteMemory create trigger memories_ai");

    // After delete: remove old row from FTS
    const char* trigger_delete =
        "CREATE TRIGGER IF NOT EXISTS memories_ad AFTER DELETE ON memories BEGIN"
        "  INSERT INTO memories_fts(memories_fts, rowid, key, content)"
        "  VALUES ('delete', old.rowid, old.key, old.content);"
        "END;";
    exec_or_throw(db_, trigger_delete, "SqliteMemory create trigger memories_ad");

    // After update: update FTS (delete old, insert new)
    const char* trigger_update =
        "CREATE TRIGGER IF NOT EXISTS memories_au AFTER UPDATE ON memories BEGIN"
        "  INSERT INTO memories_fts(memories_fts, rowid, key, content)"
        "  VALUES ('delete', old.rowid, old.key, old.content);"
        "  INSERT INTO memories_fts(rowid, key, content)"
        "  VALUES (new.rowid, new.key, new.content);"
        "END;";
    exec_or_throw(db_, trigger_update, "SqliteMemory create trigger memories_au");

    // Links table for knowledge graph
    const char* create_links =
        "CREATE TABLE IF NOT EXISTS memory_links ("
        "  from_key TEXT NOT NULL,"
        "  to_key   TEXT NOT NULL,"
        "  PRIMARY KEY (from_key, to_key)"
        ");";
    exec_or_throw(db_, create_links, "SqliteMemory create memory_links table");
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

// Run a recall query: prepare, bind text params + limit, step, collect MemoryEntry results.
// score_col is the 0-based column index for score, or -1 for no score column.
// negate_score flips the sign (for bm25 which returns negative values).
static std::vector<MemoryEntry> run_recall_query(
    sqlite3* db, const std::string& sql,
    const std::vector<std::string>& text_params,
    int limit, int score_col, bool negate_score) {

    StmtGuard g;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &g.stmt, nullptr) != SQLITE_OK) {
        return {};
    }

    int col = 1;
    for (const auto& p : text_params) {
        sqlite3_bind_text(g.stmt, col++, p.c_str(), -1, SQLITE_TRANSIENT);
    }
    sqlite3_bind_int(g.stmt, col, limit);

    std::vector<MemoryEntry> results;
    int rc = sqlite3_step(g.stmt);
    while (rc == SQLITE_ROW) {
        auto entry = entry_from_stmt(g.stmt);
        if (score_col >= 0) {
            double s = sqlite3_column_double(g.stmt, score_col);
            entry.score = negate_score ? -s : s;
        }
        results.push_back(std::move(entry));
        rc = sqlite3_step(g.stmt);
    }
    return results;
}

void SqliteMemory::populate_links(MemoryEntry& entry) {
    StmtGuard g;
    const char* sql = "SELECT to_key FROM memory_links WHERE from_key = ?;";
    if (sqlite3_prepare_v2(db_, sql, -1, &g.stmt, nullptr) != SQLITE_OK) return;
    sqlite3_bind_text(g.stmt, 1, entry.key.c_str(), -1, SQLITE_STATIC);
    while (sqlite3_step(g.stmt) == SQLITE_ROW) {
        if (auto* v = sqlite3_column_text(g.stmt, 0)) {
            entry.links.emplace_back(reinterpret_cast<const char*>(v));
        }
    }
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

    if (query.empty()) return {};

    int lim = static_cast<int>(limit);

    // Try FTS5 MATCH first (OR-joined tokens for partial matching)
    std::string fts_query = build_fts_query(query);
    if (fts_query.empty()) fts_query = query;  // fallback if all tokens too short
    std::string fts_sql =
        "SELECT m.id, m.key, m.content, m.category, m.timestamp, m.session_id,"
        "       bm25(memories_fts) AS score"
        " FROM memories_fts"
        " JOIN memories AS m ON memories_fts.rowid = m.rowid"
        " WHERE memories_fts MATCH ?";
    std::vector<std::string> fts_params = {fts_query};
    if (category_filter) {
        fts_sql += " AND m.category = ?";
        fts_params.push_back(category_to_string(*category_filter));
    }
    fts_sql += " ORDER BY bm25(memories_fts) LIMIT ?;";

    auto results = run_recall_query(db_, fts_sql, fts_params, lim, 6, true);

    if (results.empty()) {
        // Fall back to LIKE search on key and content
        std::string like_pat = "%" + query + "%";
        std::string like_sql =
            "SELECT id, key, content, category, timestamp, session_id"
            " FROM memories"
            " WHERE (key LIKE ? OR content LIKE ?)";
        std::vector<std::string> like_params = {like_pat, like_pat};
        if (category_filter) {
            like_sql += " AND category = ?";
            like_params.push_back(category_to_string(*category_filter));
        }
        like_sql += " ORDER BY timestamp DESC LIMIT ?;";

        results = run_recall_query(db_, like_sql, like_params, lim, -1, false);
    }

    for (auto& entry : results) {
        populate_links(entry);
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
        auto entry = entry_from_stmt(g.stmt);
        populate_links(entry);
        return entry;
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
        auto entry = entry_from_stmt(g.stmt);
        populate_links(entry);
        results.push_back(std::move(entry));
        rc = sqlite3_step(g.stmt);
    }
    return results;
}

bool SqliteMemory::forget(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Delete links referencing this key
    {
        StmtGuard lg;
        const char* link_sql = "DELETE FROM memory_links WHERE from_key = ? OR to_key = ?;";
        if (sqlite3_prepare_v2(db_, link_sql, -1, &lg.stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(lg.stmt, 1, key.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(lg.stmt, 2, key.c_str(), -1, SQLITE_STATIC);
            sqlite3_step(lg.stmt);
        }
    }

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
        populate_links(entry);
        arr.push_back(entry_to_json(entry));
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
            auto entry = entry_from_json(item);
            if (entry.key.empty()) continue;

            if (entry.id.empty()) entry.id = generate_id();
            if (entry.timestamp == 0) entry.timestamp = epoch_seconds();
            std::string cat = category_to_string(entry.category);

            StmtGuard g;
            if (sqlite3_prepare_v2(db_, sql, -1, &g.stmt, nullptr) != SQLITE_OK) continue;

            auto ts = static_cast<int64_t>(entry.timestamp);
            sqlite3_bind_text(g.stmt, 1, entry.id.c_str(),         -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(g.stmt, 2, entry.key.c_str(),        -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(g.stmt, 3, entry.content.c_str(),    -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(g.stmt, 4, cat.c_str(),              -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(g.stmt, 5, ts);
            sqlite3_bind_text(g.stmt, 6, entry.session_id.c_str(), -1, SQLITE_TRANSIENT);

            if (sqlite3_step(g.stmt) == SQLITE_DONE && sqlite3_changes(db_) > 0) {
                imported++;

                // Import links for this entry
                for (const auto& to : entry.links) {
                    StmtGuard lg;
                    const char* link_sql =
                        "INSERT OR IGNORE INTO memory_links (from_key, to_key) VALUES (?, ?);";
                    if (sqlite3_prepare_v2(db_, link_sql, -1, &lg.stmt, nullptr) == SQLITE_OK) {
                        sqlite3_bind_text(lg.stmt, 1, entry.key.c_str(), -1, SQLITE_TRANSIENT);
                        sqlite3_bind_text(lg.stmt, 2, to.c_str(),        -1, SQLITE_TRANSIENT);
                        sqlite3_step(lg.stmt);
                    }
                }
            }
        }
    } catch (...) { // NOLINT(bugprone-empty-catch)
    }
    return imported;
}

uint32_t SqliteMemory::hygiene_purge(uint32_t max_age_seconds) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto cutoff = static_cast<int64_t>(epoch_seconds()) - static_cast<int64_t>(max_age_seconds);

    // Clean up links referencing entries about to be purged
    {
        StmtGuard lg;
        const char* link_sql =
            "DELETE FROM memory_links WHERE from_key IN "
            "(SELECT key FROM memories WHERE category = 'conversation' AND timestamp <= ?) "
            "OR to_key IN "
            "(SELECT key FROM memories WHERE category = 'conversation' AND timestamp <= ?);";
        if (sqlite3_prepare_v2(db_, link_sql, -1, &lg.stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(lg.stmt, 1, cutoff);
            sqlite3_bind_int64(lg.stmt, 2, cutoff);
            sqlite3_step(lg.stmt);
        }
    }

    StmtGuard g;
    const char* sql =
        "DELETE FROM memories WHERE category = 'conversation' AND timestamp <= ?;";
    if (sqlite3_prepare_v2(db_, sql, -1, &g.stmt, nullptr) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_int64(g.stmt, 1, cutoff);
    sqlite3_step(g.stmt);

    return static_cast<uint32_t>(sqlite3_changes(db_));
}

bool SqliteMemory::link(const std::string& from_key, const std::string& to_key) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Verify both keys exist
    auto key_exists = [this](const std::string& key) -> bool {
        StmtGuard g;
        const char* sql = "SELECT 1 FROM memories WHERE key = ?;";
        if (sqlite3_prepare_v2(db_, sql, -1, &g.stmt, nullptr) != SQLITE_OK) return false;
        sqlite3_bind_text(g.stmt, 1, key.c_str(), -1, SQLITE_STATIC);
        return sqlite3_step(g.stmt) == SQLITE_ROW;
    };

    if (!key_exists(from_key) || !key_exists(to_key)) return false;

    // Insert both directions
    const char* sql = "INSERT OR IGNORE INTO memory_links (from_key, to_key) VALUES (?, ?);";

    {
        StmtGuard g;
        if (sqlite3_prepare_v2(db_, sql, -1, &g.stmt, nullptr) != SQLITE_OK) return false;
        sqlite3_bind_text(g.stmt, 1, from_key.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(g.stmt, 2, to_key.c_str(), -1, SQLITE_STATIC);
        sqlite3_step(g.stmt);
    }
    {
        StmtGuard g;
        if (sqlite3_prepare_v2(db_, sql, -1, &g.stmt, nullptr) != SQLITE_OK) return false;
        sqlite3_bind_text(g.stmt, 1, to_key.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(g.stmt, 2, from_key.c_str(), -1, SQLITE_STATIC);
        sqlite3_step(g.stmt);
    }

    return true;
}

bool SqliteMemory::unlink(const std::string& from_key, const std::string& to_key) {
    std::lock_guard<std::mutex> lock(mutex_);

    const char* sql = "DELETE FROM memory_links WHERE "
                      "(from_key = ? AND to_key = ?) OR (from_key = ? AND to_key = ?);";
    StmtGuard g;
    if (sqlite3_prepare_v2(db_, sql, -1, &g.stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(g.stmt, 1, from_key.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(g.stmt, 2, to_key.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(g.stmt, 3, to_key.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(g.stmt, 4, from_key.c_str(), -1, SQLITE_STATIC);
    sqlite3_step(g.stmt);

    return sqlite3_changes(db_) > 0;
}

std::vector<MemoryEntry> SqliteMemory::neighbors(const std::string& key, uint32_t limit) {
    std::lock_guard<std::mutex> lock(mutex_);

    const char* sql =
        "SELECT m.id, m.key, m.content, m.category, m.timestamp, m.session_id"
        " FROM memories m"
        " JOIN memory_links l ON m.key = l.to_key"
        " WHERE l.from_key = ? LIMIT ?;";

    StmtGuard g;
    if (sqlite3_prepare_v2(db_, sql, -1, &g.stmt, nullptr) != SQLITE_OK) return {};
    sqlite3_bind_text(g.stmt, 1, key.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(g.stmt, 2, static_cast<int>(limit));

    std::vector<MemoryEntry> results;
    while (sqlite3_step(g.stmt) == SQLITE_ROW) {
        auto entry = entry_from_stmt(g.stmt);
        populate_links(entry);
        results.push_back(std::move(entry));
    }
    return results;
}

} // namespace ptrclaw
