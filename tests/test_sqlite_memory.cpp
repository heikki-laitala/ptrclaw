#include <catch2/catch_test_macros.hpp>
#include "memory/sqlite_memory.hpp"
#include <filesystem>

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

// ── Backend name ─────────────────────────────────────────────

TEST_CASE("SqliteMemory: backend_name returns sqlite", "[sqlite_memory]") {
    SqliteFixture f;
    REQUIRE(f.mem.backend_name() == "sqlite");
}
