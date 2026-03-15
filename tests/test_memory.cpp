#include <catch2/catch_test_macros.hpp>
#include "memory.hpp"
#include "memory/json_memory.hpp"
#include "memory/none_memory.hpp"
#include <filesystem>
#include <unistd.h>

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

// ── collect_neighbors ─────────────────────────────────────────

TEST_CASE("collect_neighbors returns empty for null memory", "[memory]") {
    std::vector<MemoryEntry> entries;
    MemoryEntry e;
    e.key = "test";
    e.links = {"other"};
    entries.push_back(e);
    auto result = collect_neighbors(nullptr, entries, 10);
    REQUIRE(result.empty());
}

TEST_CASE("collect_neighbors returns empty for empty entries", "[memory]") {
    NoneMemory mem;
    std::vector<MemoryEntry> entries;
    auto result = collect_neighbors(&mem, entries, 10);
    REQUIRE(result.empty());
}

// ── memory_enrich Core exclusion ────────────────────────────────

TEST_CASE("memory_enrich excludes Core entries from context block", "[memory]") {
    std::string path = "/tmp/ptrclaw_test_enrich_" + std::to_string(getpid()) + ".json";
    {
        JsonMemory mem(path);
        mem.store("soul:identity", "I am an assistant", MemoryCategory::Core, "");
        mem.store("user-likes-python", "User prefers Python", MemoryCategory::Knowledge, "");

        // Query that matches both entries
        std::string result = memory_enrich(&mem, "assistant Python", 10);

        // Knowledge entry should be in context
        REQUIRE(result.find("user-likes-python") != std::string::npos);
        // Core entry should NOT be in context (it's in system prompt)
        REQUIRE(result.find("soul:identity") == std::string::npos);
    }
    std::filesystem::remove(path);
}

// ── memory_enrich layered context (PER-390) ──────────────────────

TEST_CASE("memory_enrich: concepts and observations in separate sections", "[memory][per390]") {
    std::string path = "/tmp/ptrclaw_test_layered_" + std::to_string(getpid()) + ".json";
    {
        JsonMemory mem(path);
        // Concept: empty session_id (cross-session)
        mem.store("pref:rust", "User prefers Rust", MemoryCategory::Knowledge, "");
        // Observation: non-empty session_id (session-scoped)
        mem.store("user-debugging", "User is debugging a segfault", MemoryCategory::Knowledge, "sess1");

        std::string result = memory_enrich(&mem, "rust segfault debugging", 10);

        auto concepts_pos = result.find("Concepts:");
        auto obs_pos = result.find("Observations:");
        auto rust_pos = result.find("pref:rust");
        auto debug_pos = result.find("user-debugging");

        REQUIRE(concepts_pos != std::string::npos);
        REQUIRE(obs_pos != std::string::npos);
        REQUIRE(rust_pos != std::string::npos);
        REQUIRE(debug_pos != std::string::npos);
        // pref:rust is a concept so it appears before Observations section
        REQUIRE(rust_pos > concepts_pos);
        REQUIRE(rust_pos < obs_pos);
        // user-debugging is an observation so it appears after Observations header
        REQUIRE(debug_pos > obs_pos);
    }
    std::filesystem::remove(path);
}

TEST_CASE("memory_enrich: only Concepts section when all entries are cross-session", "[memory][per390]") {
    std::string path = "/tmp/ptrclaw_test_concepts_only_" + std::to_string(getpid()) + ".json";
    {
        JsonMemory mem(path);
        mem.store("pref:vim", "Prefers Vim", MemoryCategory::Knowledge, "");
        mem.store("pref:dark-mode", "Uses dark mode", MemoryCategory::Knowledge, "");

        std::string result = memory_enrich(&mem, "vim dark mode editor", 10);

        REQUIRE(result.find("Concepts:") != std::string::npos);
        REQUIRE(result.find("Observations:") == std::string::npos);
    }
    std::filesystem::remove(path);
}

TEST_CASE("memory_enrich: only Observations section when all entries have session_id", "[memory][per390]") {
    std::string path = "/tmp/ptrclaw_test_obs_only_" + std::to_string(getpid()) + ".json";
    {
        JsonMemory mem(path);
        mem.store("task-current", "Debugging auth module", MemoryCategory::Knowledge, "session-abc");

        std::string result = memory_enrich(&mem, "auth module session debugging", 10);

        REQUIRE(result.find("Observations:") != std::string::npos);
        REQUIRE(result.find("Concepts:") == std::string::npos);
    }
    std::filesystem::remove(path);
}

TEST_CASE("memory_enrich: episode_context appears in block", "[memory][per390]") {
    NoneMemory mem;
    std::string result = memory_enrich(&mem, "hello", 5, 0, "Past episodes: episode:0 (5 turns)");

    REQUIRE(result.find("[Memory context]") != std::string::npos);
    REQUIRE(result.find("Past episodes: episode:0") != std::string::npos);
    REQUIRE(result.find("[/Memory context]") != std::string::npos);
    REQUIRE(result.find("hello") != std::string::npos);
}

TEST_CASE("memory_enrich: episode_context shown even with null memory", "[memory][per390]") {
    std::string result = memory_enrich(nullptr, "hello", 5, 0, "Past episodes: episode:0 (3 turns)");

    REQUIRE(result.find("[Memory context]") != std::string::npos);
    REQUIRE(result.find("Past episodes:") != std::string::npos);
    REQUIRE(result.find("hello") != std::string::npos);
}

TEST_CASE("memory_enrich: no episode_context and no entries returns original message", "[memory][per390]") {
    NoneMemory mem;
    std::string result = memory_enrich(&mem, "hello", 5, 0, "");
    REQUIRE(result == "hello");
}

TEST_CASE("memory_enrich: episode_context combined with entries shows all sections", "[memory][per390]") {
    std::string path = "/tmp/ptrclaw_test_combined_" + std::to_string(getpid()) + ".json";
    {
        JsonMemory mem(path);
        mem.store("pref:cpp", "User prefers C++", MemoryCategory::Knowledge, "");

        std::string result = memory_enrich(&mem, "c++ programming", 10, 0,
                                           "Past episodes: episode:0 (4 turns)");

        REQUIRE(result.find("Concepts:") != std::string::npos);
        REQUIRE(result.find("pref:cpp") != std::string::npos);
        REQUIRE(result.find("Past episodes: episode:0") != std::string::npos);
    }
    std::filesystem::remove(path);
}
