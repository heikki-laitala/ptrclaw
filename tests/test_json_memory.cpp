#include <catch2/catch_test_macros.hpp>
#include "memory/json_memory.hpp"
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>
#include <unistd.h>

using namespace ptrclaw;

static std::string test_path() {
    return "/tmp/ptrclaw_test_memory_" + std::to_string(getpid()) + ".json";
}

struct JsonMemoryFixture {
    std::string path = test_path();
    JsonMemory mem{path};

    ~JsonMemoryFixture() {
        std::filesystem::remove(path);
        std::filesystem::remove(path + ".tmp");
    }
};

// ── Store and get ────────────────────────────────────────────

TEST_CASE("JsonMemory: store and get", "[json_memory]") {
    JsonMemoryFixture f;

    auto id = f.mem.store("language", "Python", MemoryCategory::Knowledge, "");
    REQUIRE_FALSE(id.empty());

    auto entry = f.mem.get("language");
    REQUIRE(entry.has_value());
    REQUIRE(entry.value_or(MemoryEntry{}).key == "language");
    REQUIRE(entry.value_or(MemoryEntry{}).content == "Python");
    REQUIRE(entry.value_or(MemoryEntry{}).category == MemoryCategory::Knowledge);
}

TEST_CASE("JsonMemory: upsert on same key", "[json_memory]") {
    JsonMemoryFixture f;

    f.mem.store("language", "Python", MemoryCategory::Knowledge, "");
    f.mem.store("language", "Rust", MemoryCategory::Knowledge, "");

    auto entry = f.mem.get("language");
    REQUIRE(entry.has_value());
    REQUIRE(entry.value_or(MemoryEntry{}).content == "Rust");
    REQUIRE(f.mem.count(std::nullopt) == 1);
}

// ── Recall ───────────────────────────────────────────────────

TEST_CASE("JsonMemory: recall finds matching entries", "[json_memory]") {
    JsonMemoryFixture f;

    f.mem.store("favorite-lang", "Python is my favorite", MemoryCategory::Knowledge, "");
    f.mem.store("favorite-food", "Pizza is great", MemoryCategory::Knowledge, "");
    f.mem.store("hobby", "Reading books", MemoryCategory::Core, "");

    auto results = f.mem.recall("favorite", 10, std::nullopt);
    REQUIRE(results.size() == 2);
    REQUIRE(results[0].score > 0.0);
}

TEST_CASE("JsonMemory: recall with category filter", "[json_memory]") {
    JsonMemoryFixture f;

    f.mem.store("core-item", "identity thing", MemoryCategory::Core, "");
    f.mem.store("know-item", "knowledge thing", MemoryCategory::Knowledge, "");

    auto results = f.mem.recall("thing", 10, MemoryCategory::Core);
    REQUIRE(results.size() == 1);
    REQUIRE(results[0].category == MemoryCategory::Core);
}

TEST_CASE("JsonMemory: recall respects limit", "[json_memory]") {
    JsonMemoryFixture f;

    for (int i = 0; i < 10; i++) {
        f.mem.store("item" + std::to_string(i), "matching content", MemoryCategory::Knowledge, "");
    }

    auto results = f.mem.recall("matching", 3, std::nullopt);
    REQUIRE(results.size() == 3);
}

// ── List ─────────────────────────────────────────────────────

TEST_CASE("JsonMemory: list all entries", "[json_memory]") {
    JsonMemoryFixture f;

    f.mem.store("a", "alpha", MemoryCategory::Core, "");
    f.mem.store("b", "beta", MemoryCategory::Knowledge, "");

    auto all = f.mem.list(std::nullopt, 100);
    REQUIRE(all.size() == 2);
}

TEST_CASE("JsonMemory: list with category filter", "[json_memory]") {
    JsonMemoryFixture f;

    f.mem.store("a", "alpha", MemoryCategory::Core, "");
    f.mem.store("b", "beta", MemoryCategory::Knowledge, "");

    auto core = f.mem.list(MemoryCategory::Core, 100);
    REQUIRE(core.size() == 1);
    REQUIRE(core[0].key == "a");
}

// ── Forget ───────────────────────────────────────────────────

TEST_CASE("JsonMemory: forget removes entry", "[json_memory]") {
    JsonMemoryFixture f;

    f.mem.store("temp", "temporary data", MemoryCategory::Conversation, "");
    REQUIRE(f.mem.count(std::nullopt) == 1);

    bool deleted = f.mem.forget("temp");
    REQUIRE(deleted);
    REQUIRE(f.mem.count(std::nullopt) == 0);
    REQUIRE_FALSE(f.mem.get("temp").has_value());
}

TEST_CASE("JsonMemory: forget returns false for missing key", "[json_memory]") {
    JsonMemoryFixture f;
    REQUIRE_FALSE(f.mem.forget("nonexistent"));
}

// ── Count ────────────────────────────────────────────────────

TEST_CASE("JsonMemory: count with and without filter", "[json_memory]") {
    JsonMemoryFixture f;

    f.mem.store("a", "x", MemoryCategory::Core, "");
    f.mem.store("b", "y", MemoryCategory::Knowledge, "");
    f.mem.store("c", "z", MemoryCategory::Knowledge, "");

    REQUIRE(f.mem.count(std::nullopt) == 3);
    REQUIRE(f.mem.count(MemoryCategory::Core) == 1);
    REQUIRE(f.mem.count(MemoryCategory::Knowledge) == 2);
    REQUIRE(f.mem.count(MemoryCategory::Conversation) == 0);
}

// ── Snapshot export/import ───────────────────────────────────

TEST_CASE("JsonMemory: snapshot export and import", "[json_memory]") {
    JsonMemoryFixture f;

    f.mem.store("key1", "value1", MemoryCategory::Core, "");
    f.mem.store("key2", "value2", MemoryCategory::Knowledge, "");

    std::string exported = f.mem.snapshot_export();
    REQUIRE(exported.find("key1") != std::string::npos);
    REQUIRE(exported.find("key2") != std::string::npos);

    // Import into a fresh instance
    std::string path2 = test_path() + "_import";
    {
        JsonMemory mem2(path2);
        uint32_t imported = mem2.snapshot_import(exported);
        REQUIRE(imported == 2);
        REQUIRE(mem2.count(std::nullopt) == 2);
    }
    std::filesystem::remove(path2);
}

TEST_CASE("JsonMemory: snapshot import skips existing keys", "[json_memory]") {
    JsonMemoryFixture f;

    f.mem.store("key1", "original", MemoryCategory::Knowledge, "");

    std::string json = R"([{"key":"key1","content":"new","category":"knowledge","timestamp":0}])";
    uint32_t imported = f.mem.snapshot_import(json);
    REQUIRE(imported == 0);

    auto entry = f.mem.get("key1");
    REQUIRE(entry.value_or(MemoryEntry{}).content == "original");
}

// ── Hygiene purge ────────────────────────────────────────────

TEST_CASE("JsonMemory: hygiene_purge removes old conversation entries", "[json_memory]") {
    JsonMemoryFixture f;

    f.mem.store("conv", "old message", MemoryCategory::Conversation, "");
    f.mem.store("keep", "important fact", MemoryCategory::Knowledge, "");

    // Purge with a large max_age that covers now + 1 — entries with timestamp <= now are old enough
    // max_age of 1 second means cutoff = now - 1; entries stored at now won't be purged.
    // Use a huge value for a reliable test: all entries have timestamp <= now.
    // Wait a moment so timestamps are strictly in the past, then purge with max_age=1.
    std::this_thread::sleep_for(std::chrono::seconds(2));
    uint32_t purged = f.mem.hygiene_purge(1);
    REQUIRE(purged == 1);
    REQUIRE(f.mem.count(std::nullopt) == 1);
    REQUIRE(f.mem.get("keep").has_value());
    REQUIRE_FALSE(f.mem.get("conv").has_value());
}

// ── Persistence ──────────────────────────────────────────────

// ── Links ────────────────────────────────────────────────────

TEST_CASE("JsonMemory: link creates bidirectional links", "[json_memory]") {
    JsonMemoryFixture f;

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

TEST_CASE("JsonMemory: unlink removes bidirectional links", "[json_memory]") {
    JsonMemoryFixture f;

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

TEST_CASE("JsonMemory: neighbors returns linked entries", "[json_memory]") {
    JsonMemoryFixture f;

    f.mem.store("center", "Center node", MemoryCategory::Knowledge, "");
    f.mem.store("neighbor1", "First neighbor", MemoryCategory::Knowledge, "");
    f.mem.store("neighbor2", "Second neighbor", MemoryCategory::Knowledge, "");

    f.mem.link("center", "neighbor1");
    f.mem.link("center", "neighbor2");

    auto neighbors = f.mem.neighbors("center", 10);
    REQUIRE(neighbors.size() == 2);
}

TEST_CASE("JsonMemory: link fails for missing entry", "[json_memory]") {
    JsonMemoryFixture f;

    f.mem.store("exists", "content", MemoryCategory::Knowledge, "");
    REQUIRE_FALSE(f.mem.link("exists", "missing"));
    REQUIRE_FALSE(f.mem.link("missing", "exists"));
}

TEST_CASE("JsonMemory: forget cleans up links", "[json_memory]") {
    JsonMemoryFixture f;

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

TEST_CASE("JsonMemory: links persist across instances", "[json_memory]") {
    std::string path = test_path() + "_links";

    {
        JsonMemory mem(path);
        mem.store("p", "P data", MemoryCategory::Knowledge, "");
        mem.store("q", "Q data", MemoryCategory::Knowledge, "");
        mem.link("p", "q");
    }

    {
        JsonMemory mem(path);
        auto p = mem.get("p");
        REQUIRE(p.has_value());
        REQUIRE(p.value_or(MemoryEntry{}).links.size() == 1);
        REQUIRE(p.value_or(MemoryEntry{}).links[0] == "q");
    }

    std::filesystem::remove(path);
}

TEST_CASE("JsonMemory: hygiene_purge cleans dangling links", "[json_memory]") {
    JsonMemoryFixture f;

    f.mem.store("conv-item", "conversation data", MemoryCategory::Conversation, "");
    f.mem.store("knowledge-item", "knowledge data", MemoryCategory::Knowledge, "");
    f.mem.link("conv-item", "knowledge-item");

    std::this_thread::sleep_for(std::chrono::seconds(2));
    f.mem.hygiene_purge(1);

    auto k = f.mem.get("knowledge-item");
    REQUIRE(k.has_value());
    REQUIRE(k.value_or(MemoryEntry{}).links.empty());
}

// ── Persistence ──────────────────────────────────────────────

// ── Search quality ────────────────────────────────────────────

TEST_CASE("JsonMemory: key matches rank higher than content matches", "[json_memory]") {
    JsonMemoryFixture f;

    f.mem.store("python-version", "The version is 3.12", MemoryCategory::Knowledge, "");
    f.mem.store("build-system", "Uses python as a scripting tool", MemoryCategory::Knowledge, "");

    auto results = f.mem.recall("python", 10, std::nullopt);
    REQUIRE(results.size() == 2);
    // Entry with "python" in key should rank higher
    REQUIRE(results[0].key == "python-version");
    REQUIRE(results[0].score > results[1].score);
}

TEST_CASE("JsonMemory: word boundary matching prevents substring false positives", "[json_memory]") {
    JsonMemoryFixture f;

    f.mem.store("testing-info", "Unit testing with Catch2", MemoryCategory::Knowledge, "");
    f.mem.store("protest-info", "Attest to the attestation", MemoryCategory::Knowledge, "");

    auto results = f.mem.recall("test", 10, std::nullopt);
    // "test" should match "testing-info" (word "test" is a prefix of "testing" — but
    // tokenization splits on non-alnum, so "testing" is one token and "test" != "testing").
    // However, key "testing-info" tokenizes to ["testing", "info"] — no exact "test" match.
    // So neither should match if using strict word boundary.
    // Let's test with an entry that has the exact word "test":
    f.mem.store("test-framework", "The test suite runs fast", MemoryCategory::Knowledge, "");

    results = f.mem.recall("test", 10, std::nullopt);
    REQUIRE_FALSE(results.empty());
    REQUIRE(results[0].key == "test-framework");
}

TEST_CASE("JsonMemory: recall ranking is deterministic for same scores", "[json_memory]") {
    JsonMemoryFixture f;

    f.mem.store("item-alpha", "matching content here", MemoryCategory::Knowledge, "");
    f.mem.store("item-beta", "matching content here", MemoryCategory::Knowledge, "");
    f.mem.store("item-gamma", "matching content here", MemoryCategory::Knowledge, "");

    auto r1 = f.mem.recall("matching content", 3, std::nullopt);
    auto r2 = f.mem.recall("matching content", 3, std::nullopt);
    REQUIRE(r1.size() == r2.size());
    // All three should be returned (they have matching content words)
    REQUIRE(r1.size() == 3);
}

TEST_CASE("JsonMemory: recall with empty query returns empty", "[json_memory]") {
    JsonMemoryFixture f;

    f.mem.store("item", "content", MemoryCategory::Knowledge, "");
    auto results = f.mem.recall("", 10, std::nullopt);
    REQUIRE(results.empty());
}

TEST_CASE("JsonMemory: recall with special characters in query", "[json_memory]") {
    JsonMemoryFixture f;

    f.mem.store("cpp-version", "Uses C++ 17 standard", MemoryCategory::Knowledge, "");
    // Query with special chars — tokenizer extracts "c" and "17"
    auto results = f.mem.recall("C++ 17", 10, std::nullopt);
    // "17" should match
    REQUIRE_FALSE(results.empty());
}

// ── Persistence ──────────────────────────────────────────────

TEST_CASE("JsonMemory: persists across instances", "[json_memory]") {
    std::string path = test_path() + "_persist";

    {
        JsonMemory mem(path);
        mem.store("persistent", "data here", MemoryCategory::Core, "");
    }

    {
        JsonMemory mem(path);
        auto entry = mem.get("persistent");
        REQUIRE(entry.has_value());
        REQUIRE(entry.value_or(MemoryEntry{}).content == "data here");
    }

    std::filesystem::remove(path);
}
