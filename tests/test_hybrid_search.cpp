#include <catch2/catch_test_macros.hpp>
#include "embedder.hpp"
#include "memory/json_memory.hpp"
#include <nlohmann/json.hpp>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <unistd.h>

#ifdef PTRCLAW_HAS_SQLITE_MEMORY
#include "memory/sqlite_memory.hpp"
#include <sqlite3.h>
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

TEST_CASE("JsonMemory hybrid: entries without embeddings get no vector contribution", "[hybrid][json_memory]") {
    std::string path = json_test_path();
    {
        // Create memory WITHOUT embedder — entry has no embedding stored
        JsonMemory mem(path);
        mem.store("no-emb", "completely unrelated stuff", MemoryCategory::Knowledge, "");
    }
    {
        // Reopen WITH embedder
        SemanticMockEmbedder embedder;
        JsonMemory mem(path);
        mem.set_embedder(&embedder, 0.4, 0.6);

        // Store one entry WITH embeddings (semantic match for "feline")
        mem.store("with-emb", "cat kitten fluffy", MemoryCategory::Knowledge, "");

        // Query "feline" — no text match in either entry, but "with-emb" has
        // a vector match (cat/kitten/feline map to same mock embedding dimension).
        // The non-embedded entry must NOT surface — it has no text match and
        // must not receive an artificial vector score from the query embedding.
        auto results = mem.recall("feline", 5, std::nullopt);
        REQUIRE(results.size() == 1);
        REQUIRE(results[0].key == "with-emb");
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

// ── JsonMemory recency decay ──────────────────────────────────

TEST_CASE("JsonMemory: recency decay boosts recent entries", "[recency][json_memory]") {
    std::string path = json_test_path();
    {
        JsonMemory mem(path);
        // Store two entries with same content but different timestamps.
        // We manually store, then overwrite timestamps via the file.
        mem.store("old-cat", "I love cats and kittens", MemoryCategory::Knowledge, "");
        mem.store("new-cat", "I love cats and kittens", MemoryCategory::Knowledge, "");
    }
    {
        // Reopen and manually adjust timestamps in JSON
        std::ifstream in(path);
        auto j = nlohmann::json::parse(in);
        in.close();

        uint64_t now = static_cast<uint64_t>(std::time(nullptr));
        // Old entry: 30 days ago
        j[0]["timestamp"] = now - 30 * 86400;
        // New entry: 1 minute ago
        j[1]["timestamp"] = now - 60;

        std::ofstream out(path);
        out << j.dump(2);
    }
    {
        JsonMemory mem(path);
        mem.set_recency_decay(86400); // 1-day half-life

        auto results = mem.recall("cats", 5, std::nullopt);
        REQUIRE(results.size() == 2);
        // New entry should rank first due to recency decay
        REQUIRE(results[0].key == "new-cat");
        // And have a higher score
        REQUIRE(results[0].score > results[1].score);
    }
    std::filesystem::remove(path);
    std::filesystem::remove(path + ".tmp");
}

TEST_CASE("JsonMemory: recency decay disabled when half_life is 0", "[recency][json_memory]") {
    std::string path = json_test_path();
    {
        JsonMemory mem(path);
        mem.store("entry-a", "Python programming code", MemoryCategory::Knowledge, "");
        mem.store("entry-b", "Python programming code", MemoryCategory::Knowledge, "");
    }
    {
        std::ifstream in(path);
        auto j = nlohmann::json::parse(in);
        in.close();

        uint64_t now = static_cast<uint64_t>(std::time(nullptr));
        j[0]["timestamp"] = now - 30 * 86400;
        j[1]["timestamp"] = now - 60;

        std::ofstream out(path);
        out << j.dump(2);
    }
    {
        JsonMemory mem(path);
        // No set_recency_decay — default is 0 (disabled)

        auto results = mem.recall("Python", 5, std::nullopt);
        REQUIRE(results.size() == 2);
        // Without decay, both should have the same score
        REQUIRE(std::abs(results[0].score - results[1].score) < 1e-6);
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

TEST_CASE("SqliteMemory: recency decay boosts recent entries", "[recency][sqlite_memory]") {
    std::string path = sqlite_hybrid_path();
    {
        SqliteMemory mem(path);
        mem.set_recency_decay(86400); // 1-day half-life

        mem.store("old-python", "Python programming code", MemoryCategory::Knowledge, "");
        mem.store("new-python", "Python programming code", MemoryCategory::Knowledge, "");

        // Manually update timestamps via SQL
        sqlite3* db = nullptr;
        sqlite3_open(path.c_str(), &db);
        uint64_t now = static_cast<uint64_t>(std::time(nullptr));
        std::string sql1 = "UPDATE memories SET timestamp = " +
                           std::to_string(now - 30 * 86400) + " WHERE key = 'old-python';";
        std::string sql2 = "UPDATE memories SET timestamp = " +
                           std::to_string(now - 60) + " WHERE key = 'new-python';";
        sqlite3_exec(db, sql1.c_str(), nullptr, nullptr, nullptr);
        sqlite3_exec(db, sql2.c_str(), nullptr, nullptr, nullptr);
        sqlite3_close(db);

        auto results = mem.recall("Python", 5, std::nullopt);
        REQUIRE(results.size() == 2);
        REQUIRE(results[0].key == "new-python");
        REQUIRE(results[0].score > results[1].score);
    }
    std::filesystem::remove(path);
    std::filesystem::remove(path + "-wal");
    std::filesystem::remove(path + "-shm");
}

#endif // PTRCLAW_HAS_SQLITE_MEMORY
