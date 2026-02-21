#include <catch2/catch_test_macros.hpp>
#include "memory/embeddings.hpp"
#include "memory/vector.hpp"

using namespace ptrclaw;

// ── NoopEmbedding ────────────────────────────────────────────

TEST_CASE("NoopEmbedding: returns empty vector", "[embeddings]") {
    NoopEmbedding noop;
    auto vec = noop.embed("hello world");
    REQUIRE(vec.empty());
}

TEST_CASE("NoopEmbedding: dimensions is 0", "[embeddings]") {
    NoopEmbedding noop;
    REQUIRE(noop.dimensions() == 0);
}

TEST_CASE("NoopEmbedding: name is none", "[embeddings]") {
    NoopEmbedding noop;
    REQUIRE(noop.name() == "none");
}

// ── Vector utilities ─────────────────────────────────────────

TEST_CASE("cosine_similarity: identical vectors return 1.0", "[vector]") {
    std::vector<float> a = {1.0f, 2.0f, 3.0f};
    double sim = cosine_similarity(a, a);
    REQUIRE(sim > 0.999);
    REQUIRE(sim <= 1.001);
}

TEST_CASE("cosine_similarity: orthogonal vectors return 0.0", "[vector]") {
    std::vector<float> a = {1.0f, 0.0f};
    std::vector<float> b = {0.0f, 1.0f};
    double sim = cosine_similarity(a, b);
    REQUIRE(sim < 0.001);
    REQUIRE(sim >= -0.001);
}

TEST_CASE("cosine_similarity: empty vectors return 0.0", "[vector]") {
    std::vector<float> empty;
    REQUIRE(cosine_similarity(empty, empty) == 0.0);
}

TEST_CASE("cosine_similarity: different lengths return 0.0", "[vector]") {
    std::vector<float> a = {1.0f, 2.0f};
    std::vector<float> b = {1.0f};
    REQUIRE(cosine_similarity(a, b) == 0.0);
}

// ── Serialization ────────────────────────────────────────────

TEST_CASE("serialize/deserialize round-trip", "[vector]") {
    std::vector<float> original = {1.5f, -2.3f, 0.0f, 42.0f};
    std::string data = serialize_vector(original);
    auto restored = deserialize_vector(data);

    REQUIRE(restored.size() == original.size());
    for (size_t i = 0; i < original.size(); i++) {
        REQUIRE(restored[i] == original[i]);
    }
}

TEST_CASE("deserialize empty string returns empty vector", "[vector]") {
    auto vec = deserialize_vector("");
    REQUIRE(vec.empty());
}

// ── Hybrid merge ─────────────────────────────────────────────

TEST_CASE("hybrid_merge: combines keyword and vector results", "[vector]") {
    std::vector<ScoredResult> keyword = {
        {"key1", 0.8},
        {"key2", 0.5},
    };
    std::vector<ScoredResult> vector_r = {
        {"key1", 0.9},
        {"key3", 0.7},
    };

    auto merged = hybrid_merge(keyword, vector_r, 0.3, 0.7, 10);
    REQUIRE_FALSE(merged.empty());

    // key1 should be highest (appears in both)
    REQUIRE(merged[0].key == "key1");
    // All 3 keys should appear
    REQUIRE(merged.size() == 3);
}

TEST_CASE("hybrid_merge: respects limit", "[vector]") {
    std::vector<ScoredResult> keyword = {
        {"a", 0.9}, {"b", 0.8}, {"c", 0.7},
    };
    std::vector<ScoredResult> vector_r = {
        {"d", 0.6}, {"e", 0.5},
    };

    auto merged = hybrid_merge(keyword, vector_r, 0.5, 0.5, 2);
    REQUIRE(merged.size() == 2);
}

TEST_CASE("hybrid_merge: empty inputs return empty", "[vector]") {
    std::vector<ScoredResult> empty;
    auto merged = hybrid_merge(empty, empty, 0.5, 0.5, 10);
    REQUIRE(merged.empty());
}
