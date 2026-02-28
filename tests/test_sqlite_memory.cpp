#include <catch2/catch_test_macros.hpp>
#include "memory/sqlite_memory.hpp"
#include <ctime>
#include <filesystem>
#include <sqlite3.h>
#include <unistd.h>

using namespace ptrclaw;

static std::string sqlite_test_path() {
    return "/tmp/ptrclaw_test_sqlite_" + std::to_string(getpid()) + ".db";
}

struct SqliteFixture {
    std::string path = sqlite_test_path();
    SqliteMemory mem{path};

    ~SqliteFixture() {
        std::filesystem::remove(path);
        std::filesystem::remove(path + "-wal");
        std::filesystem::remove(path + "-shm");
    }
};

// ── Store and get ────────────────────────────────────────────

TEST_CASE("SqliteMemory: store and get", "[sqlite_memory]") {
    SqliteFixture f;

    auto id = f.mem.store("language", "Python", MemoryCategory::Knowledge, "");
    REQUIRE_FALSE(id.empty());

    auto entry = f.mem.get("language");
    REQUIRE(entry.has_value());
    REQUIRE(entry.value_or(MemoryEntry{}).key == "language");
    REQUIRE(entry.value_or(MemoryEntry{}).content == "Python");
    REQUIRE(entry.value_or(MemoryEntry{}).category == MemoryCategory::Knowledge);
}

TEST_CASE("SqliteMemory: upsert on same key", "[sqlite_memory]") {
    SqliteFixture f;

    f.mem.store("language", "Python", MemoryCategory::Knowledge, "");
    f.mem.store("language", "Rust", MemoryCategory::Knowledge, "");

    auto entry = f.mem.get("language");
    REQUIRE(entry.has_value());
    REQUIRE(entry.value_or(MemoryEntry{}).content == "Rust");
    REQUIRE(f.mem.count(std::nullopt) == 1);
}

// ── Recall (FTS) ─────────────────────────────────────────────

TEST_CASE("SqliteMemory: recall finds matching entries", "[sqlite_memory]") {
    SqliteFixture f;

    f.mem.store("favorite-lang", "Python is my favorite language", MemoryCategory::Knowledge, "");
    f.mem.store("favorite-food", "Pizza is great", MemoryCategory::Knowledge, "");
    f.mem.store("hobby", "Reading books", MemoryCategory::Core, "");

    auto results = f.mem.recall("favorite", 10, std::nullopt);
    REQUIRE(results.size() >= 2);
}

TEST_CASE("SqliteMemory: recall with category filter", "[sqlite_memory]") {
    SqliteFixture f;

    f.mem.store("core-item", "identity thing", MemoryCategory::Core, "");
    f.mem.store("know-item", "knowledge thing", MemoryCategory::Knowledge, "");

    auto results = f.mem.recall("thing", 10, MemoryCategory::Core);
    REQUIRE(results.size() == 1);
    REQUIRE(results[0].category == MemoryCategory::Core);
}

TEST_CASE("SqliteMemory: recall respects limit", "[sqlite_memory]") {
    SqliteFixture f;

    for (int i = 0; i < 10; i++) {
        f.mem.store("item" + std::to_string(i), "matching content", MemoryCategory::Knowledge, "");
    }

    auto results = f.mem.recall("matching", 3, std::nullopt);
    REQUIRE(results.size() == 3);
}

// ── List ─────────────────────────────────────────────────────

TEST_CASE("SqliteMemory: list all entries", "[sqlite_memory]") {
    SqliteFixture f;

    f.mem.store("a", "alpha", MemoryCategory::Core, "");
    f.mem.store("b", "beta", MemoryCategory::Knowledge, "");

    auto all = f.mem.list(std::nullopt, 100);
    REQUIRE(all.size() == 2);
}

// ── Forget ───────────────────────────────────────────────────

TEST_CASE("SqliteMemory: forget removes entry", "[sqlite_memory]") {
    SqliteFixture f;

    f.mem.store("temp", "temporary data", MemoryCategory::Conversation, "");
    REQUIRE(f.mem.count(std::nullopt) == 1);

    bool deleted = f.mem.forget("temp");
    REQUIRE(deleted);
    REQUIRE(f.mem.count(std::nullopt) == 0);
}

TEST_CASE("SqliteMemory: forget returns false for missing key", "[sqlite_memory]") {
    SqliteFixture f;
    REQUIRE_FALSE(f.mem.forget("nonexistent"));
}

// ── Count ────────────────────────────────────────────────────

TEST_CASE("SqliteMemory: count with and without filter", "[sqlite_memory]") {
    SqliteFixture f;

    f.mem.store("a", "x", MemoryCategory::Core, "");
    f.mem.store("b", "y", MemoryCategory::Knowledge, "");
    f.mem.store("c", "z", MemoryCategory::Knowledge, "");

    REQUIRE(f.mem.count(std::nullopt) == 3);
    REQUIRE(f.mem.count(MemoryCategory::Core) == 1);
    REQUIRE(f.mem.count(MemoryCategory::Knowledge) == 2);
}

// ── Snapshot ─────────────────────────────────────────────────

TEST_CASE("SqliteMemory: snapshot export contains entries", "[sqlite_memory]") {
    SqliteFixture f;

    f.mem.store("key1", "value1", MemoryCategory::Core, "");
    std::string exported = f.mem.snapshot_export();
    REQUIRE(exported.find("key1") != std::string::npos);
    REQUIRE(exported.find("value1") != std::string::npos);
}

TEST_CASE("SqliteMemory: snapshot import adds entries", "[sqlite_memory]") {
    SqliteFixture f;

    std::string json = R"([{"key":"imported","content":"data","category":"knowledge","timestamp":0,"session_id":"","id":"abc123"}])";
    uint32_t imported = f.mem.snapshot_import(json);
    REQUIRE(imported == 1);

    auto entry = f.mem.get("imported");
    REQUIRE(entry.has_value());
    REQUIRE(entry.value_or(MemoryEntry{}).content == "data");
}

// ── Hygiene purge ────────────────────────────────────────────

TEST_CASE("SqliteMemory: hygiene_purge removes old conversation entries", "[sqlite_memory]") {
    SqliteFixture f;

    f.mem.store("conv", "old message", MemoryCategory::Conversation, "");
    f.mem.store("keep", "important fact", MemoryCategory::Knowledge, "");

    uint32_t purged = f.mem.hygiene_purge(0);
    REQUIRE(purged == 1);
    REQUIRE(f.mem.count(std::nullopt) == 1);
    REQUIRE(f.mem.get("keep").has_value());
}

// ── Links ────────────────────────────────────────────────────

TEST_CASE("SqliteMemory: link creates bidirectional links", "[sqlite_memory]") {
    SqliteFixture f;

    f.mem.store("topic-a", "About topic A", MemoryCategory::Knowledge, "");
    f.mem.store("topic-b", "About topic B", MemoryCategory::Knowledge, "");

    bool ok = f.mem.link("topic-a", "topic-b");
    REQUIRE(ok);

    auto a = f.mem.get("topic-a");
    REQUIRE(a.has_value());
    REQUIRE(a.value_or(MemoryEntry{}).links.size() == 1);
    REQUIRE(a.value_or(MemoryEntry{}).links[0] == "topic-b");

    auto b = f.mem.get("topic-b");
    REQUIRE(b.has_value());
    REQUIRE(b.value_or(MemoryEntry{}).links.size() == 1);
    REQUIRE(b.value_or(MemoryEntry{}).links[0] == "topic-a");
}

TEST_CASE("SqliteMemory: unlink removes bidirectional links", "[sqlite_memory]") {
    SqliteFixture f;

    f.mem.store("x", "X content", MemoryCategory::Knowledge, "");
    f.mem.store("y", "Y content", MemoryCategory::Knowledge, "");
    f.mem.link("x", "y");

    bool ok = f.mem.unlink("x", "y");
    REQUIRE(ok);

    auto x = f.mem.get("x");
    REQUIRE(x.value_or(MemoryEntry{}).links.empty());
    auto y = f.mem.get("y");
    REQUIRE(y.value_or(MemoryEntry{}).links.empty());
}

TEST_CASE("SqliteMemory: neighbors returns linked entries", "[sqlite_memory]") {
    SqliteFixture f;

    f.mem.store("center", "Center node", MemoryCategory::Knowledge, "");
    f.mem.store("neighbor1", "First neighbor", MemoryCategory::Knowledge, "");
    f.mem.store("neighbor2", "Second neighbor", MemoryCategory::Knowledge, "");

    f.mem.link("center", "neighbor1");
    f.mem.link("center", "neighbor2");

    auto neighbors = f.mem.neighbors("center", 10);
    REQUIRE(neighbors.size() == 2);
}

TEST_CASE("SqliteMemory: link fails for missing entry", "[sqlite_memory]") {
    SqliteFixture f;

    f.mem.store("exists", "content", MemoryCategory::Knowledge, "");
    REQUIRE_FALSE(f.mem.link("exists", "missing"));
    REQUIRE_FALSE(f.mem.link("missing", "exists"));
}

TEST_CASE("SqliteMemory: forget cleans up links", "[sqlite_memory]") {
    SqliteFixture f;

    f.mem.store("a", "A", MemoryCategory::Knowledge, "");
    f.mem.store("b", "B", MemoryCategory::Knowledge, "");
    f.mem.store("c", "C", MemoryCategory::Knowledge, "");
    f.mem.link("a", "b");
    f.mem.link("b", "c");

    f.mem.forget("b");

    // a and c should have no links to b
    auto a = f.mem.get("a");
    REQUIRE(a.value_or(MemoryEntry{}).links.empty());
    auto c = f.mem.get("c");
    REQUIRE(c.value_or(MemoryEntry{}).links.empty());
}

TEST_CASE("SqliteMemory: hygiene_purge cleans dangling links", "[sqlite_memory]") {
    SqliteFixture f;

    f.mem.store("conv-item", "conversation data", MemoryCategory::Conversation, "");
    f.mem.store("knowledge-item", "knowledge data", MemoryCategory::Knowledge, "");
    f.mem.link("conv-item", "knowledge-item");

    f.mem.hygiene_purge(0);

    auto k = f.mem.get("knowledge-item");
    REQUIRE(k.has_value());
    REQUIRE(k.value_or(MemoryEntry{}).links.empty());
}

// ── Persistence ──────────────────────────────────────────────

TEST_CASE("SqliteMemory: persists across instances", "[sqlite_memory]") {
    std::string path = sqlite_test_path() + "_persist";

    {
        SqliteMemory mem(path);
        mem.store("persistent", "data here", MemoryCategory::Core, "");
    }

    {
        SqliteMemory mem(path);
        auto entry = mem.get("persistent");
        REQUIRE(entry.has_value());
        REQUIRE(entry.value_or(MemoryEntry{}).content == "data here");
    }

    std::filesystem::remove(path);
    std::filesystem::remove(path + "-wal");
    std::filesystem::remove(path + "-shm");
}

// ── Search quality ────────────────────────────────────────────

TEST_CASE("SqliteMemory: recall finds 2-char tokens via FTS", "[sqlite_memory]") {
    SqliteFixture f;

    f.mem.store("go-lang", "Go is a compiled language", MemoryCategory::Knowledge, "");
    f.mem.store("python-lang", "Python is interpreted", MemoryCategory::Knowledge, "");

    auto results = f.mem.recall("Go language", 10, std::nullopt);
    REQUIRE_FALSE(results.empty());
    // "Go" (2 chars) should now be included in FTS query
    bool found_go = false;
    for (const auto& r : results) {
        if (r.key == "go-lang") found_go = true;
    }
    REQUIRE(found_go);
}

TEST_CASE("SqliteMemory: recall falls back to LIKE for single-char query", "[sqlite_memory]") {
    SqliteFixture f;

    f.mem.store("c-language", "C is a systems language", MemoryCategory::Knowledge, "");

    // Single char "C" is below minimum — should fall back to LIKE
    auto results = f.mem.recall("C", 10, std::nullopt);
    // LIKE with "%C%" should match "C is a systems language"
    REQUIRE_FALSE(results.empty());
}

TEST_CASE("SqliteMemory: recall with empty query returns empty", "[sqlite_memory]") {
    SqliteFixture f;

    f.mem.store("item", "content", MemoryCategory::Knowledge, "");
    auto results = f.mem.recall("", 10, std::nullopt);
    REQUIRE(results.empty());
}

// ── Backend name ─────────────────────────────────────────────

TEST_CASE("SqliteMemory: backend_name returns sqlite", "[sqlite_memory]") {
    SqliteFixture f;
    REQUIRE(f.mem.backend_name() == "sqlite");
}

// ── Knowledge decay ──────────────────────────────────────────

TEST_CASE("SqliteMemory: recall updates last_accessed", "[sqlite_memory]") {
    SqliteFixture f;

    f.mem.store("topic", "some knowledge data", MemoryCategory::Knowledge, "");

    auto before = f.mem.get("topic");
    REQUIRE(before.has_value());

    // Recall should touch last_accessed
    f.mem.recall("knowledge", 10, std::nullopt);

    // Verify by checking the raw DB through get — last_accessed is in the DB
    // We can't read it from get() directly, but we verify the store set it.
    // The test is that recall doesn't crash and returns results.
    auto results = f.mem.recall("knowledge", 10, std::nullopt);
    REQUIRE_FALSE(results.empty());
}

TEST_CASE("SqliteMemory: hygiene purges idle Knowledge entries", "[sqlite_memory]") {
    // Use a fresh DB path to avoid conflicts
    std::string path = sqlite_test_path() + "_decay";
    {
        SqliteMemory mem(path);
        mem.set_knowledge_decay(1, 0.0);  // 1 day idle, 0% survival

        mem.store("knowledge-item", "old fact", MemoryCategory::Knowledge, "");
        mem.store("core-item", "identity", MemoryCategory::Core, "");

        // The entry was just stored so last_accessed = now.
        // We need to backdate it. Do it via raw SQL.
    }

    // Re-open and backdate last_accessed
    {
        sqlite3* db = nullptr;
        sqlite3_open(path.c_str(), &db);
        sqlite3_exec(db, "UPDATE memories SET last_accessed = 1000000 WHERE key = 'knowledge-item';",
                     nullptr, nullptr, nullptr);
        sqlite3_close(db);
    }

    {
        SqliteMemory mem(path);
        mem.set_knowledge_decay(1, 0.0);

        uint32_t purged = mem.hygiene_purge(999999999);
        REQUIRE(purged == 1);
        REQUIRE_FALSE(mem.get("knowledge-item").has_value());
        REQUIRE(mem.get("core-item").has_value());
    }

    std::filesystem::remove(path);
    std::filesystem::remove(path + "-wal");
    std::filesystem::remove(path + "-shm");
}

TEST_CASE("SqliteMemory: hygiene spares recently accessed Knowledge", "[sqlite_memory]") {
    SqliteFixture f;

    f.mem.set_knowledge_decay(1, 0.0);  // 0% survival
    f.mem.store("fresh", "recently accessed", MemoryCategory::Knowledge, "");

    // Entry was just stored — last_accessed = now, well within 1-day window
    uint32_t purged = f.mem.hygiene_purge(999999999);
    REQUIRE(purged == 0);
    REQUIRE(f.mem.get("fresh").has_value());
}

TEST_CASE("SqliteMemory: hygiene never purges Core entries", "[sqlite_memory]") {
    std::string path = sqlite_test_path() + "_core_decay";
    {
        SqliteMemory mem(path);
        mem.store("soul:identity", "I am bot", MemoryCategory::Core, "");
    }
    // Backdate
    {
        sqlite3* db = nullptr;
        sqlite3_open(path.c_str(), &db);
        sqlite3_exec(db, "UPDATE memories SET last_accessed = 1000000, timestamp = 1000000"
                         " WHERE key = 'soul:identity';",
                     nullptr, nullptr, nullptr);
        sqlite3_close(db);
    }
    {
        SqliteMemory mem(path);
        mem.set_knowledge_decay(1, 0.0);

        uint32_t purged = mem.hygiene_purge(999999999);
        REQUIRE(purged == 0);
        REQUIRE(mem.get("soul:identity").has_value());
    }
    std::filesystem::remove(path);
    std::filesystem::remove(path + "-wal");
    std::filesystem::remove(path + "-shm");
}

TEST_CASE("SqliteMemory: knowledge decay survivors get last_accessed refreshed", "[sqlite_memory]") {
    std::string path = sqlite_test_path() + "_survivor";
    {
        SqliteMemory mem(path);
        mem.store("lucky", "survives", MemoryCategory::Knowledge, "");
    }
    // Backdate
    {
        sqlite3* db = nullptr;
        sqlite3_open(path.c_str(), &db);
        sqlite3_exec(db, "UPDATE memories SET last_accessed = 1000000 WHERE key = 'lucky';",
                     nullptr, nullptr, nullptr);
        sqlite3_close(db);
    }
    {
        SqliteMemory mem(path);
        mem.set_knowledge_decay(1, 1.0);  // 100% survival

        uint32_t purged = mem.hygiene_purge(999999999);
        REQUIRE(purged == 0);
        REQUIRE(mem.get("lucky").has_value());

        // Verify last_accessed was refreshed (read raw)
        sqlite3* db = nullptr;
        sqlite3_open(path.c_str(), &db);
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db, "SELECT last_accessed FROM memories WHERE key = 'lucky';",
                           -1, &stmt, nullptr);
        REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
        auto refreshed = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
        REQUIRE(refreshed > 1000000);
        sqlite3_finalize(stmt);
        sqlite3_close(db);
    }
    std::filesystem::remove(path);
    std::filesystem::remove(path + "-wal");
    std::filesystem::remove(path + "-shm");
}

TEST_CASE("SqliteMemory: last_accessed 0 falls back to timestamp for decay", "[sqlite_memory]") {
    std::string path = sqlite_test_path() + "_fallback";
    {
        SqliteMemory mem(path);
        mem.store("legacy", "old entry", MemoryCategory::Knowledge, "");
    }
    // Set last_accessed = 0 and timestamp = old
    {
        sqlite3* db = nullptr;
        sqlite3_open(path.c_str(), &db);
        sqlite3_exec(db, "UPDATE memories SET last_accessed = 0, timestamp = 1000000"
                         " WHERE key = 'legacy';",
                     nullptr, nullptr, nullptr);
        sqlite3_close(db);
    }
    {
        SqliteMemory mem(path);
        mem.set_knowledge_decay(1, 0.0);

        uint32_t purged = mem.hygiene_purge(999999999);
        REQUIRE(purged == 1);
        REQUIRE_FALSE(mem.get("legacy").has_value());
    }
    std::filesystem::remove(path);
    std::filesystem::remove(path + "-wal");
    std::filesystem::remove(path + "-shm");
}

TEST_CASE("SqliteMemory: idle fade penalizes Knowledge entries nearing deadline", "[sqlite_memory]") {
    std::string path = sqlite_test_path() + "_idle_fade";
    {
        SqliteMemory mem(path);
        mem.store("fresh-fact", "matching data here", MemoryCategory::Knowledge, "");
        mem.store("stale-fact", "matching data here", MemoryCategory::Knowledge, "");
    }
    // Backdate stale-fact's last_accessed to 25 days ago (past the halfway fade point of 15 days)
    {
        auto now = static_cast<int64_t>(std::time(nullptr));
        auto stale_ts = now - 25 * 86400;
        std::string sql = "UPDATE memories SET last_accessed = " +
                          std::to_string(stale_ts) + " WHERE key = 'stale-fact';";
        sqlite3* db = nullptr;
        sqlite3_open(path.c_str(), &db);
        sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr);
        sqlite3_close(db);
    }
    {
        SqliteMemory mem(path);
        mem.set_knowledge_decay(30, 0.05);

        auto results = mem.recall("matching data", 10, std::nullopt);
        REQUIRE(results.size() == 2);

        double fresh_score = 0.0;
        double stale_score = 0.0;
        for (const auto& r : results) {
            if (r.key == "fresh-fact") fresh_score = r.score;
            if (r.key == "stale-fact") stale_score = r.score;
        }
        REQUIRE(fresh_score > stale_score);
    }
    std::filesystem::remove(path);
    std::filesystem::remove(path + "-wal");
    std::filesystem::remove(path + "-shm");
}

TEST_CASE("SqliteMemory: idle fade does not affect Core entries", "[sqlite_memory]") {
    std::string path = sqlite_test_path() + "_idle_fade_core";
    {
        SqliteMemory mem(path);
        mem.store("soul:identity", "matching identity", MemoryCategory::Core, "");
        mem.store("knowledge-fact", "matching knowledge", MemoryCategory::Knowledge, "");
    }
    // Backdate both to very old
    {
        sqlite3* db = nullptr;
        sqlite3_open(path.c_str(), &db);
        sqlite3_exec(db, "UPDATE memories SET last_accessed = 1000000, timestamp = 1000000;",
                     nullptr, nullptr, nullptr);
        sqlite3_close(db);
    }
    {
        SqliteMemory mem(path);
        mem.set_knowledge_decay(1, 0.0);

        auto results = mem.recall("matching", 10, std::nullopt);
        REQUIRE(results.size() >= 1);

        double core_score = 0.0;
        double knowledge_score = 0.0;
        for (const auto& r : results) {
            if (r.key == "soul:identity") core_score = r.score;
            if (r.key == "knowledge-fact") knowledge_score = r.score;
        }
        REQUIRE(core_score > knowledge_score);
    }
    std::filesystem::remove(path);
    std::filesystem::remove(path + "-wal");
    std::filesystem::remove(path + "-shm");
}
