#include "sqlite_memory.hpp"
#include "../config.hpp"
#include "entry_json.hpp"
#include "../plugin.hpp"
#include "../util.hpp"
#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include <algorithm>
#include <cstring>
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

    // Add embedding column (silently ignored if it already exists)
    sqlite3_exec(db_, "ALTER TABLE memories ADD COLUMN embedding BLOB;",
                 nullptr, nullptr, nullptr);

    // Add last_accessed column (silently ignored if it already exists)
    sqlite3_exec(db_, "ALTER TABLE memories ADD COLUMN last_accessed INTEGER;",
                 nullptr, nullptr, nullptr);

    // Links table for knowledge graph
    const char* create_links =
        "CREATE TABLE IF NOT EXISTS memory_links ("
        "  from_key TEXT NOT NULL,"
        "  to_key   TEXT NOT NULL,"
        "  PRIMARY KEY (from_key, to_key)"
        ");";
    sqlite3_exec(db_, create_links, nullptr, nullptr, nullptr);
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

void SqliteMemory::set_embedder(Embedder* embedder, double text_weight,
                                 double vector_weight) {
    embedder_ = embedder;
    text_weight_ = text_weight;
    vector_weight_ = vector_weight;
}

void SqliteMemory::set_recency_decay(uint32_t half_life_seconds) {
    recency_half_life_ = half_life_seconds;
}

void SqliteMemory::set_knowledge_decay(uint32_t max_idle_days, double survival_chance) {
    knowledge_max_idle_days_ = max_idle_days;
    knowledge_survival_chance_ = survival_chance;
}

void SqliteMemory::apply_config(const MemoryConfig& cfg) {
    set_recency_decay(cfg.recency_half_life);
    set_knowledge_decay(cfg.knowledge_max_idle_days, cfg.knowledge_survival_chance);
}

void SqliteMemory::touch_last_accessed(const std::vector<MemoryEntry>& entries) {
    if (entries.empty()) return;
    auto now = static_cast<int64_t>(epoch_seconds());

    // Build single UPDATE with IN (...) clause
    std::string sql = "UPDATE memories SET last_accessed = ? WHERE key IN (";
    for (size_t i = 0; i < entries.size(); i++) {
        if (i > 0) sql += ',';
        sql += '?';
    }
    sql += ");";

    StmtGuard g;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &g.stmt, nullptr) != SQLITE_OK) return;
    sqlite3_bind_int64(g.stmt, 1, now);
    for (size_t i = 0; i < entries.size(); i++) {
        sqlite3_bind_text(g.stmt, static_cast<int>(i + 2),
                          entries[i].key.c_str(), -1, SQLITE_TRANSIENT);
    }
    sqlite3_step(g.stmt);
}

void SqliteMemory::apply_idle_fade(std::vector<MemoryEntry>& entries) {
    if (knowledge_max_idle_days_ == 0 || entries.empty()) return;

    // Collect Knowledge entry keys
    std::vector<size_t> knowledge_indices;
    for (size_t i = 0; i < entries.size(); i++) {
        if (entries[i].category == MemoryCategory::Knowledge) {
            knowledge_indices.push_back(i);
        }
    }
    if (knowledge_indices.empty()) return;

    // Batch-fetch last_accessed for Knowledge entries
    std::string sql = "SELECT key, COALESCE(NULLIF(last_accessed, 0), timestamp)"
                      " FROM memories WHERE key IN (";
    for (size_t i = 0; i < knowledge_indices.size(); i++) {
        if (i > 0) sql += ',';
        sql += '?';
    }
    sql += ");";

    std::unordered_map<std::string, uint64_t> access_times;
    {
        StmtGuard sg;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &sg.stmt, nullptr) == SQLITE_OK) {
            for (size_t i = 0; i < knowledge_indices.size(); i++) {
                sqlite3_bind_text(sg.stmt, static_cast<int>(i + 1),
                                  entries[knowledge_indices[i]].key.c_str(),
                                  -1, SQLITE_TRANSIENT);
            }
            while (sqlite3_step(sg.stmt) == SQLITE_ROW) {
                if (auto* v = sqlite3_column_text(sg.stmt, 0)) {
                    std::string key = reinterpret_cast<const char*>(v);
                    auto ts = static_cast<uint64_t>(sqlite3_column_int64(sg.stmt, 1));
                    access_times[key] = ts;
                }
            }
        }
    }

    // Apply idle fade multiplier
    uint64_t now = epoch_seconds();
    auto max_idle = static_cast<uint64_t>(knowledge_max_idle_days_) * 86400;
    bool needs_resort = false;
    for (size_t idx : knowledge_indices) {
        auto it = access_times.find(entries[idx].key);
        if (it == access_times.end()) continue;
        uint64_t idle = (now > it->second) ? now - it->second : 0;
        double fade = idle_fade(idle, max_idle);
        if (fade < 1.0) {
            entries[idx].score *= fade;
            needs_resort = true;
        }
    }

    if (needs_resort) {
        std::sort(entries.begin(), entries.end(),
                  [](const MemoryEntry& a, const MemoryEntry& b) {
                      return a.score > b.score;
                  });
    }
}

std::string SqliteMemory::store(const std::string& key, const std::string& content,
                                 MemoryCategory category, const std::string& session_id) {
    // Compute embedding OUTSIDE the mutex (HTTP call may be slow)
    Embedding emb;
    if (embedder_) {
        emb = embedder_->embed(key + " " + content);
    }

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

    // Set last_accessed = now
    {
        StmtGuard lg;
        const char* la_sql = "UPDATE memories SET last_accessed = ? WHERE key = ?;";
        if (sqlite3_prepare_v2(db_, la_sql, -1, &lg.stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(lg.stmt, 1, ts);
            sqlite3_bind_text(lg.stmt, 2, key.c_str(), -1, SQLITE_STATIC);
            sqlite3_step(lg.stmt);
        }
    }

    // Store embedding as BLOB if computed
    if (!emb.empty()) {
        StmtGuard eg;
        const char* emb_sql = "UPDATE memories SET embedding = ? WHERE key = ?;";
        if (sqlite3_prepare_v2(db_, emb_sql, -1, &eg.stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_blob(eg.stmt, 1, emb.data(),
                              static_cast<int>(emb.size() * sizeof(float)),
                              SQLITE_STATIC);
            sqlite3_bind_text(eg.stmt, 2, key.c_str(), -1, SQLITE_STATIC);
            sqlite3_step(eg.stmt);
        }
    }

    return id;
}

// Helper: read embedding BLOB from a column into a vector<float>
static Embedding read_embedding_blob(sqlite3_stmt* stmt, int col) {
    const void* blob = sqlite3_column_blob(stmt, col);
    int bytes = sqlite3_column_bytes(stmt, col);
    if (!blob || bytes <= 0) return {};

    size_t count = static_cast<size_t>(bytes) / sizeof(float);
    Embedding emb(count);
    std::memcpy(emb.data(), blob, static_cast<size_t>(bytes));
    return emb;
}

std::vector<MemoryEntry> SqliteMemory::recall(const std::string& query, uint32_t limit,
                                               std::optional<MemoryCategory> category_filter) {
    // Compute query embedding OUTSIDE the mutex (HTTP call may be slow)
    Embedding query_emb;
    if (embedder_) {
        query_emb = embedder_->embed(query);
    }

    std::lock_guard<std::mutex> lock(mutex_);

    if (query.empty()) return {};

    bool has_vector = !query_emb.empty();

    if (!has_vector) {
        // No embedder — use original text-only search
        int lim = static_cast<int>(limit);

        std::string fts_query = build_fts_query(query);
        if (fts_query.empty()) fts_query = query;
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

        // Apply recency decay and re-sort
        if (recency_half_life_ > 0 && !results.empty()) {
            uint64_t now = epoch_seconds();
            for (auto& entry : results) {
                uint64_t age = (now > entry.timestamp) ? now - entry.timestamp : 0;
                entry.score *= recency_decay(age, recency_half_life_);
            }
            std::sort(results.begin(), results.end(),
                      [](const MemoryEntry& a, const MemoryEntry& b) {
                          return a.score > b.score;
                      });
        }

        // Apply idle fade and touch last_accessed
        apply_idle_fade(results);
        touch_last_accessed(results);

        for (auto& entry : results) {
            populate_links(entry);
        }
        return results;
    }

    // Hybrid search: scan all entries, compute BM25 + cosine hybrid score

    // Step 1: get FTS5 BM25 scores for text matching
    std::unordered_map<std::string, double> bm25_scores;
    double max_bm25 = 0.0;
    {
        std::string fts_query = build_fts_query(query);
        if (fts_query.empty()) fts_query = query;
        std::string fts_sql =
            "SELECT m.key, -bm25(memories_fts) AS score"
            " FROM memories_fts"
            " JOIN memories AS m ON memories_fts.rowid = m.rowid"
            " WHERE memories_fts MATCH ?";
        std::vector<std::string> fts_params = {fts_query};
        if (category_filter) {
            fts_sql += " AND m.category = ?";
            fts_params.push_back(category_to_string(*category_filter));
        }
        fts_sql += ";";

        StmtGuard g;
        if (sqlite3_prepare_v2(db_, fts_sql.c_str(), -1, &g.stmt, nullptr) == SQLITE_OK) {
            int col = 1;
            for (const auto& p : fts_params) {
                sqlite3_bind_text(g.stmt, col++, p.c_str(), -1, SQLITE_TRANSIENT);
            }
            while (sqlite3_step(g.stmt) == SQLITE_ROW) {
                if (auto* v = sqlite3_column_text(g.stmt, 0)) {
                    std::string key = reinterpret_cast<const char*>(v);
                    double score = sqlite3_column_double(g.stmt, 1);
                    bm25_scores[key] = score;
                    if (score > max_bm25) max_bm25 = score;
                }
            }
        }
    }

    // Step 2: scan all entries for hybrid scoring
    std::string scan_sql =
        "SELECT id, key, content, category, timestamp, session_id, embedding"
        " FROM memories";
    if (category_filter) {
        scan_sql += " WHERE category = ?";
    }
    scan_sql += ";";

    StmtGuard g;
    if (sqlite3_prepare_v2(db_, scan_sql.c_str(), -1, &g.stmt, nullptr) != SQLITE_OK) {
        return {};
    }
    if (category_filter) {
        std::string cat = category_to_string(*category_filter);
        sqlite3_bind_text(g.stmt, 1, cat.c_str(), -1, SQLITE_TRANSIENT);
    }

    struct ScoredEntry {
        MemoryEntry entry;
        double score = 0.0;
    };
    std::vector<ScoredEntry> scored;

    bool has_text = !bm25_scores.empty();
    uint64_t now = epoch_seconds();

    while (sqlite3_step(g.stmt) == SQLITE_ROW) {
        auto entry = entry_from_stmt(g.stmt);
        Embedding emb = read_embedding_blob(g.stmt, 6);

        double text_norm = 0.0;
        auto bm_it = bm25_scores.find(entry.key);
        if (bm_it != bm25_scores.end() && max_bm25 > 0.0) {
            text_norm = bm_it->second / max_bm25;
        }

        double cosine_sim = 0.0;
        if (!emb.empty()) {
            cosine_sim = cosine_similarity(query_emb, emb);
        }

        double combined = hybrid_score(text_norm, cosine_sim,
                                       text_weight_, vector_weight_,
                                       has_text, !emb.empty());
        if (recency_half_life_ > 0) {
            uint64_t age = (now > entry.timestamp) ? now - entry.timestamp : 0;
            combined *= recency_decay(age, recency_half_life_);
        }
        if (combined > 0.0) {
            scored.push_back({std::move(entry), combined});
        }
    }

    // Sort by score descending, take top K
    size_t k = std::min(static_cast<size_t>(limit), scored.size());
    std::partial_sort(scored.begin(), scored.begin() + static_cast<ptrdiff_t>(k), scored.end(),
                      [](const ScoredEntry& a, const ScoredEntry& b) {
                          return a.score > b.score;
                      });

    std::vector<MemoryEntry> results;
    for (size_t i = 0; i < k; i++) {
        scored[i].entry.score = scored[i].score;
        populate_links(scored[i].entry);
        results.push_back(std::move(scored[i].entry));
    }

    // Apply idle fade and touch last_accessed
    apply_idle_fade(results);
    touch_last_accessed(results);

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

    auto now = static_cast<int64_t>(epoch_seconds());
    auto conv_cutoff = now - static_cast<int64_t>(max_age_seconds);
    uint32_t total_purged = 0;

    // Clean up links referencing conversation entries about to be purged
    {
        StmtGuard lg;
        const char* link_sql =
            "DELETE FROM memory_links WHERE from_key IN "
            "(SELECT key FROM memories WHERE category = 'conversation' AND timestamp <= ?) "
            "OR to_key IN "
            "(SELECT key FROM memories WHERE category = 'conversation' AND timestamp <= ?);";
        if (sqlite3_prepare_v2(db_, link_sql, -1, &lg.stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(lg.stmt, 1, conv_cutoff);
            sqlite3_bind_int64(lg.stmt, 2, conv_cutoff);
            sqlite3_step(lg.stmt);
        }
    }

    // Purge old conversation entries
    {
        StmtGuard g;
        const char* sql =
            "DELETE FROM memories WHERE category = 'conversation' AND timestamp <= ?;";
        if (sqlite3_prepare_v2(db_, sql, -1, &g.stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(g.stmt, 1, conv_cutoff);
            sqlite3_step(g.stmt);
            total_purged += static_cast<uint32_t>(sqlite3_changes(db_));
        }
    }

    // Knowledge decay: purge idle Knowledge entries with random survival
    if (knowledge_max_idle_days_ > 0) {
        auto knowledge_cutoff = now - static_cast<int64_t>(knowledge_max_idle_days_) * 86400;

        // Select eligible Knowledge entries (idle beyond cutoff)
        // Use COALESCE: last_accessed if set, else timestamp
        const char* select_sql =
            "SELECT key FROM memories"
            " WHERE category = 'knowledge'"
            " AND COALESCE(NULLIF(last_accessed, 0), timestamp) <= ?;";

        std::vector<std::string> to_delete;
        std::vector<std::string> survivors;
        {
            StmtGuard sg;
            if (sqlite3_prepare_v2(db_, select_sql, -1, &sg.stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int64(sg.stmt, 1, knowledge_cutoff);
                while (sqlite3_step(sg.stmt) == SQLITE_ROW) {
                    if (auto* v = sqlite3_column_text(sg.stmt, 0)) {
                        std::string key = reinterpret_cast<const char*>(v);
                        double roll = dist_(rng_);
                        if (roll >= knowledge_survival_chance_) {
                            to_delete.push_back(std::move(key));
                        } else {
                            survivors.push_back(std::move(key));
                        }
                    }
                }
            }
        }

        // Delete losers (and their links)
        for (const auto& key : to_delete) {
            {
                StmtGuard lg;
                const char* link_sql =
                    "DELETE FROM memory_links WHERE from_key = ? OR to_key = ?;";
                if (sqlite3_prepare_v2(db_, link_sql, -1, &lg.stmt, nullptr) == SQLITE_OK) {
                    sqlite3_bind_text(lg.stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(lg.stmt, 2, key.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_step(lg.stmt);
                }
            }
            {
                StmtGuard dg;
                const char* del_sql = "DELETE FROM memories WHERE key = ?;";
                if (sqlite3_prepare_v2(db_, del_sql, -1, &dg.stmt, nullptr) == SQLITE_OK) {
                    sqlite3_bind_text(dg.stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_step(dg.stmt);
                    total_purged += static_cast<uint32_t>(sqlite3_changes(db_));
                }
            }
        }

        // Refresh survivors' last_accessed
        for (const auto& key : survivors) {
            StmtGuard ug;
            const char* upd_sql = "UPDATE memories SET last_accessed = ? WHERE key = ?;";
            if (sqlite3_prepare_v2(db_, upd_sql, -1, &ug.stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int64(ug.stmt, 1, now);
                sqlite3_bind_text(ug.stmt, 2, key.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(ug.stmt);
            }
        }
    }

    return total_purged;
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
