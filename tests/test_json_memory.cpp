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
