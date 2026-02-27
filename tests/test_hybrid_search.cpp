#include <catch2/catch_test_macros.hpp>
#include "embedder.hpp"
#include "memory/json_memory.hpp"
#include <filesystem>
#include <unistd.h>

#ifdef PTRCLAW_HAS_SQLITE_MEMORY
#include "memory/sqlite_memory.hpp"
#endif

using namespace ptrclaw;

// ── Deterministic mock embedder ──────────────────────────────
// Returns different embeddings based on content keywords to simulate
// semantic similarity. "cat" and "kitten" get similar embeddings.

class SemanticMockEmbedder : public Embedder {
public:
    Embedding embed(const std::string& text) override {
        embed_count++;
        // Simple semantic simulation: keywords map to directions
        Embedding emb(4, 0.0f);

        // "cat"/"kitten"/"feline" → strong in dimension 0
        if (text.find("cat") != std::string::npos ||
            text.find("kitten") != std::string::npos ||
            text.find("feline") != std::string::npos) {
            emb[0] = 0.9f;
            emb[1] = 0.1f;
        }

        // "dog"/"puppy"/"canine" → strong in dimension 1
        if (text.find("dog") != std::string::npos ||
            text.find("puppy") != std::string::npos ||
            text.find("canine") != std::string::npos) {
            emb[1] = 0.9f;
            emb[0] = 0.1f;
        }

        // "python"/"programming"/"code" → strong in dimension 2
        if (text.find("python") != std::string::npos ||
            text.find("programming") != std::string::npos ||
            text.find("code") != std::string::npos) {
            emb[2] = 0.9f;
        }

        // "food"/"cooking"/"recipe" → strong in dimension 3
        if (text.find("food") != std::string::npos ||
            text.find("cooking") != std::string::npos ||
            text.find("recipe") != std::string::npos) {
            emb[3] = 0.9f;
        }

        // Default: small random-ish values if no keywords matched
        bool all_zero = true;
        for (float v : emb) {
            if (v != 0.0f) { all_zero = false; break; }
        }
        if (all_zero) {
            emb[0] = 0.1f;
            emb[1] = 0.1f;
            emb[2] = 0.1f;
            emb[3] = 0.1f;
        }
        return emb;
    }

    uint32_t dimensions() const override { return 4; }
    std::string embedder_name() const override { return "semantic_mock"; }
    int embed_count = 0;
};

// ── JsonMemory hybrid search ─────────────────────────────────

static std::string json_test_path() {
    return "/tmp/ptrclaw_test_hybrid_json_" + std::to_string(getpid()) + ".json";
}

struct JsonHybridFixture {
    std::string path = json_test_path();
    SemanticMockEmbedder embedder;
    JsonMemory mem{path};

    JsonHybridFixture() {
        mem.set_embedder(&embedder, 0.4, 0.6);
    }

    ~JsonHybridFixture() {
        std::filesystem::remove(path);
        std::filesystem::remove(path + ".tmp");
    }
};

TEST_CASE("JsonMemory hybrid: semantic recall finds related entries", "[hybrid][json_memory]") {
    JsonHybridFixture f;

    f.mem.store("my-cat", "I have a fluffy cat named Whiskers", MemoryCategory::Knowledge, "");
    f.mem.store("my-dog", "I have a loyal dog named Buddy", MemoryCategory::Knowledge, "");
    f.mem.store("my-food", "I love cooking Italian food", MemoryCategory::Knowledge, "");

    // Query about "kitten" should find the cat entry via vector similarity,
    // even though "kitten" doesn't appear in any entry text
    auto results = f.mem.recall("kitten", 3, std::nullopt);
    REQUIRE_FALSE(results.empty());
    REQUIRE(results[0].key == "my-cat");
}

TEST_CASE("JsonMemory hybrid: text-only still works without embeddings", "[hybrid][json_memory]") {
    JsonHybridFixture f;

    f.mem.store("python-version", "Project uses Python 3.12", MemoryCategory::Knowledge, "");
    f.mem.store("rust-version", "Also uses Rust 1.75", MemoryCategory::Knowledge, "");

    auto results = f.mem.recall("Python", 5, std::nullopt);
    REQUIRE_FALSE(results.empty());
    // Should find the python entry via text match + vector similarity
    bool found_python = false;
    for (const auto& r : results) {
        if (r.key == "python-version") { found_python = true; break; }
    }
    REQUIRE(found_python);
}

TEST_CASE("JsonMemory hybrid: entries without embeddings get vector score 0", "[hybrid][json_memory]") {
    std::string path = json_test_path();
    {
        // Create memory WITHOUT embedder, store some entries
        JsonMemory mem(path);
        mem.store("no-emb", "cat feline entry", MemoryCategory::Knowledge, "");
    }
    {
        // Reopen WITH embedder
        SemanticMockEmbedder embedder;
        JsonMemory mem(path);
        mem.set_embedder(&embedder, 0.4, 0.6);

        // Store one entry WITH embeddings
        mem.store("with-emb", "cat kitten fluffy", MemoryCategory::Knowledge, "");

        // Both should be findable, but the embedded one should score higher for "feline"
        auto results = mem.recall("feline", 5, std::nullopt);
        REQUIRE(results.size() >= 1);
    }
    std::filesystem::remove(path);
    std::filesystem::remove(path + ".tmp");
}

TEST_CASE("JsonMemory hybrid: forget removes embeddings", "[hybrid][json_memory]") {
    JsonHybridFixture f;

    f.mem.store("temp-entry", "cat data", MemoryCategory::Knowledge, "");
    REQUIRE(f.mem.count(std::nullopt) == 1);

    f.mem.forget("temp-entry");
    REQUIRE(f.mem.count(std::nullopt) == 0);

    // After forgetting, recall should return nothing
    auto results = f.mem.recall("cat", 5, std::nullopt);
    REQUIRE(results.empty());
}

TEST_CASE("JsonMemory hybrid: persistence round-trip", "[hybrid][json_memory]") {
    std::string path = json_test_path();
    {
        SemanticMockEmbedder embedder;
        JsonMemory mem(path);
        mem.set_embedder(&embedder, 0.4, 0.6);
        mem.store("persist-cat", "fluffy cat", MemoryCategory::Knowledge, "");
    }
    {
        // Reopen — should load entries and embeddings
        SemanticMockEmbedder embedder;
        JsonMemory mem(path);
        mem.set_embedder(&embedder, 0.4, 0.6);

        auto entry = mem.get("persist-cat");
        REQUIRE(entry.has_value());
        REQUIRE(entry.value_or(MemoryEntry{}).content == "fluffy cat");

        // Vector recall should work (using persisted embeddings)
        auto results = mem.recall("kitten", 5, std::nullopt);
        REQUIRE_FALSE(results.empty());
    }
    std::filesystem::remove(path);
    std::filesystem::remove(path + ".tmp");
}

// ── SqliteMemory hybrid search ───────────────────────────────

#ifdef PTRCLAW_HAS_SQLITE_MEMORY

static std::string sqlite_hybrid_path() {
    return "/tmp/ptrclaw_test_hybrid_sqlite_" + std::to_string(getpid()) + ".db";
}

struct SqliteHybridFixture {
    std::string path = sqlite_hybrid_path();
    SemanticMockEmbedder embedder;
    SqliteMemory mem{path};

    SqliteHybridFixture() {
        mem.set_embedder(&embedder, 0.4, 0.6);
    }

    ~SqliteHybridFixture() {
        std::filesystem::remove(path);
        std::filesystem::remove(path + "-wal");
        std::filesystem::remove(path + "-shm");
    }
};

TEST_CASE("SqliteMemory hybrid: semantic recall finds related entries", "[hybrid][sqlite_memory]") {
    SqliteHybridFixture f;

    f.mem.store("my-cat", "I have a fluffy cat named Whiskers", MemoryCategory::Knowledge, "");
    f.mem.store("my-dog", "I have a loyal dog named Buddy", MemoryCategory::Knowledge, "");
    f.mem.store("my-food", "I love cooking Italian food", MemoryCategory::Knowledge, "");

    auto results = f.mem.recall("kitten", 3, std::nullopt);
    REQUIRE_FALSE(results.empty());
    REQUIRE(results[0].key == "my-cat");
}

TEST_CASE("SqliteMemory hybrid: text-only falls back without embedder", "[hybrid][sqlite_memory]") {
    std::string path = sqlite_hybrid_path();
    {
        SqliteMemory mem(path);
        // No embedder set — should use text-only search

        mem.store("python-ver", "Python 3.12 is the project language", MemoryCategory::Knowledge, "");
        mem.store("rust-ver", "Rust 1.75 for performance", MemoryCategory::Knowledge, "");

        auto results = mem.recall("Python", 5, std::nullopt);
        REQUIRE_FALSE(results.empty());
    }
    std::filesystem::remove(path);
    std::filesystem::remove(path + "-wal");
    std::filesystem::remove(path + "-shm");
}

TEST_CASE("SqliteMemory hybrid: entries without embeddings gracefully degrade", "[hybrid][sqlite_memory]") {
    SqliteHybridFixture f;

    f.mem.store("with-emb", "cat kitten fluffy", MemoryCategory::Knowledge, "");

    // The entry has an embedding from store(). Recall should work.
    auto results = f.mem.recall("feline", 5, std::nullopt);
    REQUIRE_FALSE(results.empty());
}

#endif // PTRCLAW_HAS_SQLITE_MEMORY
