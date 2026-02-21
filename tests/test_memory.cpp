#include <catch2/catch_test_macros.hpp>
#include "memory.hpp"
#include "memory/none_memory.hpp"

using namespace ptrclaw;

// ── Category conversions ─────────────────────────────────────

TEST_CASE("category_to_string returns correct strings", "[memory]") {
    REQUIRE(category_to_string(MemoryCategory::Core) == "core");
    REQUIRE(category_to_string(MemoryCategory::Knowledge) == "knowledge");
    REQUIRE(category_to_string(MemoryCategory::Conversation) == "conversation");
}

TEST_CASE("category_from_string parses known categories", "[memory]") {
    REQUIRE(category_from_string("core") == MemoryCategory::Core);
    REQUIRE(category_from_string("knowledge") == MemoryCategory::Knowledge);
    REQUIRE(category_from_string("conversation") == MemoryCategory::Conversation);
}

TEST_CASE("category_from_string defaults to Knowledge for unknown", "[memory]") {
    REQUIRE(category_from_string("invalid") == MemoryCategory::Knowledge);
    REQUIRE(category_from_string("") == MemoryCategory::Knowledge);
}

// ── memory_enrich ────────────────────────────────────────────

TEST_CASE("memory_enrich returns original message when memory is null", "[memory]") {
    std::string result = memory_enrich(nullptr, "hello", 5);
    REQUIRE(result == "hello");
}

TEST_CASE("memory_enrich returns original message when limit is 0", "[memory]") {
    NoneMemory mem;
    std::string result = memory_enrich(&mem, "hello", 0);
    REQUIRE(result == "hello");
}

TEST_CASE("memory_enrich returns original message when no recall results", "[memory]") {
    NoneMemory mem;
    std::string result = memory_enrich(&mem, "hello", 5);
    REQUIRE(result == "hello");
}

// ── NoneMemory ───────────────────────────────────────────────

TEST_CASE("NoneMemory: backend_name returns none", "[memory]") {
    NoneMemory mem;
    REQUIRE(mem.backend_name() == "none");
}

TEST_CASE("NoneMemory: store returns empty string", "[memory]") {
    NoneMemory mem;
    auto id = mem.store("key", "value", MemoryCategory::Knowledge, "");
    REQUIRE(id.empty());
}

TEST_CASE("NoneMemory: recall returns empty", "[memory]") {
    NoneMemory mem;
    auto results = mem.recall("query", 5, std::nullopt);
    REQUIRE(results.empty());
}

TEST_CASE("NoneMemory: get returns nullopt", "[memory]") {
    NoneMemory mem;
    REQUIRE_FALSE(mem.get("key").has_value());
}

TEST_CASE("NoneMemory: count returns 0", "[memory]") {
    NoneMemory mem;
    REQUIRE(mem.count(std::nullopt) == 0);
}

TEST_CASE("NoneMemory: forget returns false", "[memory]") {
    NoneMemory mem;
    REQUIRE_FALSE(mem.forget("key"));
}

TEST_CASE("NoneMemory: snapshot_export returns empty array", "[memory]") {
    NoneMemory mem;
    REQUIRE(mem.snapshot_export() == "[]");
}

TEST_CASE("NoneMemory: hygiene_purge returns 0", "[memory]") {
    NoneMemory mem;
    REQUIRE(mem.hygiene_purge(3600) == 0);
}

TEST_CASE("NoneMemory: link returns false", "[memory]") {
    NoneMemory mem;
    REQUIRE_FALSE(mem.link("a", "b"));
}

TEST_CASE("NoneMemory: unlink returns false", "[memory]") {
    NoneMemory mem;
    REQUIRE_FALSE(mem.unlink("a", "b"));
}

TEST_CASE("NoneMemory: neighbors returns empty", "[memory]") {
    NoneMemory mem;
    REQUIRE(mem.neighbors("a", 10).empty());
}
