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

// ── PER-395: Retrieval/ranking policy ────────────────────────────

TEST_CASE("classify_query_intent: chronological keywords", "[memory][per395]") {
    REQUIRE(classify_query_intent("what happened recently") == QueryIntent::Chronological);
    REQUIRE(classify_query_intent("what did we discuss earlier") == QueryIntent::Chronological);
    REQUIRE(classify_query_intent("show me the history") == QueryIntent::Chronological);
    REQUIRE(classify_query_intent("what happened before") == QueryIntent::Chronological);
    REQUIRE(classify_query_intent("2 days ago we talked") == QueryIntent::Chronological);
    REQUIRE(classify_query_intent("what happened yesterday") == QueryIntent::Chronological);
    REQUIRE(classify_query_intent("previously we agreed") == QueryIntent::Chronological);
}

TEST_CASE("classify_query_intent: stable keywords", "[memory][per395]") {
    REQUIRE(classify_query_intent("what do I prefer") == QueryIntent::Stable);
    REQUIRE(classify_query_intent("what is my favorite language") == QueryIntent::Stable);
    REQUIRE(classify_query_intent("I usually use vim") == QueryIntent::Stable);
    REQUIRE(classify_query_intent("I typically work with Python") == QueryIntent::Stable);
    REQUIRE(classify_query_intent("I always use dark mode") == QueryIntent::Stable);
    REQUIRE(classify_query_intent("generally speaking I like rust") == QueryIntent::Stable);
}

TEST_CASE("classify_query_intent: unknown for generic queries", "[memory][per395]") {
    REQUIRE(classify_query_intent("help me debug this") == QueryIntent::Unknown);
    REQUIRE(classify_query_intent("what is rust") == QueryIntent::Unknown);
    REQUIRE(classify_query_intent("") == QueryIntent::Unknown);
    REQUIRE(classify_query_intent("how does memory work") == QueryIntent::Unknown);
}

TEST_CASE("classify_query_intent: case-insensitive matching", "[memory][per395]") {
    REQUIRE(classify_query_intent("What Happened RECENTLY") == QueryIntent::Chronological);
    REQUIRE(classify_query_intent("I PREFER python") == QueryIntent::Stable);
}

TEST_CASE("memory_enrich: entries within each tier sorted by score descending", "[memory][per395]") {
    std::string path = "/tmp/ptrclaw_test_score_order_" + std::to_string(getpid()) + ".json";
    {
        JsonMemory mem(path);
        // High relevance to "rust coding": key contains both query tokens (2x weight each)
        // plus content matches — total token score much higher than entry B
        mem.store("rust-coding-pref", "User loves rust coding daily", MemoryCategory::Knowledge, "");
        // Lower relevance: key contains only one query token ("coding")
        mem.store("coding-note", "User does some coding work", MemoryCategory::Knowledge, "");

        std::string result = memory_enrich(&mem, "rust coding", 10);

        auto high_pos = result.find("rust-coding-pref");
        auto low_pos  = result.find("coding-note");

        REQUIRE(high_pos != std::string::npos);
        REQUIRE(low_pos  != std::string::npos);
        // Higher-scoring entry must appear before lower-scoring entry
        REQUIRE(high_pos < low_pos);
    }
    std::filesystem::remove(path);
}

TEST_CASE("memory_enrich: tier budgets cap concepts and observations independently", "[memory][per395]") {
    std::string path = "/tmp/ptrclaw_test_tier_budget_" + std::to_string(getpid()) + ".json";
    {
        JsonMemory mem(path);
        // 4 session-scoped observations
        for (int i = 0; i < 4; i++) {
            mem.store("obs:" + std::to_string(i),
                      "session observation about task " + std::to_string(i),
                      MemoryCategory::Knowledge, "sess1");
        }
        // 4 cross-session concepts
        for (int i = 0; i < 4; i++) {
            mem.store("concept:" + std::to_string(i),
                      "general knowledge about task " + std::to_string(i),
                      MemoryCategory::Knowledge, "");
        }

        // recall_limit=4, Unknown intent → concept_budget=2, obs_budget=2
        std::string result = memory_enrich(&mem, "task", 4);

        int obs_shown = 0, concept_shown = 0;
        for (int i = 0; i < 4; i++) {
            if (result.find("obs:" + std::to_string(i)) != std::string::npos) obs_shown++;
            if (result.find("concept:" + std::to_string(i)) != std::string::npos) concept_shown++;
        }

        // Each tier capped at budget (2), and both types should appear
        REQUIRE(obs_shown <= 2);
        REQUIRE(concept_shown <= 2);
        REQUIRE(obs_shown >= 1);
        REQUIRE(concept_shown >= 1);
    }
    std::filesystem::remove(path);
}

TEST_CASE("memory_enrich: chronological intent boosts observation budget", "[memory][per395]") {
    std::string path = "/tmp/ptrclaw_test_chron_budget_" + std::to_string(getpid()) + ".json";
    {
        JsonMemory mem(path);
        // 3 concepts and 3 observations, all containing query token "task"
        for (int i = 0; i < 3; i++) {
            mem.store("concept:" + std::to_string(i),
                      "knowledge task note " + std::to_string(i),
                      MemoryCategory::Knowledge, "");
            mem.store("obs:" + std::to_string(i),
                      "session task note " + std::to_string(i),
                      MemoryCategory::Knowledge, "sess1");
        }

        // recall_limit=3, Chronological → concept_budget=1, obs_budget=2
        std::string result = memory_enrich(&mem, "task recently", 3);

        int obs_shown = 0, concept_shown = 0;
        for (int i = 0; i < 3; i++) {
            if (result.find("obs:" + std::to_string(i)) != std::string::npos) obs_shown++;
            if (result.find("concept:" + std::to_string(i)) != std::string::npos) concept_shown++;
        }

        // Chronological: obs_budget=2, concept_budget=1
        REQUIRE(obs_shown <= 2);
        REQUIRE(concept_shown <= 1);
    }
    std::filesystem::remove(path);
}

TEST_CASE("memory_enrich: stable intent boosts concept budget", "[memory][per395]") {
    std::string path = "/tmp/ptrclaw_test_stable_budget_" + std::to_string(getpid()) + ".json";
    {
        JsonMemory mem(path);
        // 3 concepts and 3 observations, all containing query token "task"
        for (int i = 0; i < 3; i++) {
            mem.store("concept:" + std::to_string(i),
                      "knowledge task fact " + std::to_string(i),
                      MemoryCategory::Knowledge, "");
            mem.store("obs:" + std::to_string(i),
                      "session task fact " + std::to_string(i),
                      MemoryCategory::Knowledge, "sess1");
        }

        // recall_limit=3, Stable → concept_budget=2, obs_budget=1
        std::string result = memory_enrich(&mem, "task prefer", 3);

        int obs_shown = 0, concept_shown = 0;
        for (int i = 0; i < 3; i++) {
            if (result.find("obs:" + std::to_string(i)) != std::string::npos) obs_shown++;
            if (result.find("concept:" + std::to_string(i)) != std::string::npos) concept_shown++;
        }

        // Stable: concept_budget=2, obs_budget=1
        REQUIRE(concept_shown <= 2);
        REQUIRE(obs_shown <= 1);
    }
    std::filesystem::remove(path);
}
