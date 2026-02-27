#include <catch2/catch_test_macros.hpp>
#include "embedder.hpp"
#include <cmath>

using namespace ptrclaw;

// ── Cosine similarity ────────────────────────────────────────

TEST_CASE("cosine_similarity: identical vectors", "[embedder]") {
    Embedding a = {1.0f, 0.0f, 0.0f};
    Embedding b = {1.0f, 0.0f, 0.0f};
    REQUIRE(std::abs(cosine_similarity(a, b) - 1.0) < 1e-6);
}

TEST_CASE("cosine_similarity: orthogonal vectors", "[embedder]") {
    Embedding a = {1.0f, 0.0f, 0.0f};
    Embedding b = {0.0f, 1.0f, 0.0f};
    REQUIRE(std::abs(cosine_similarity(a, b)) < 1e-6);
}

TEST_CASE("cosine_similarity: opposite vectors", "[embedder]") {
    Embedding a = {1.0f, 0.0f, 0.0f};
    Embedding b = {-1.0f, 0.0f, 0.0f};
    REQUIRE(std::abs(cosine_similarity(a, b) + 1.0) < 1e-6);
}

TEST_CASE("cosine_similarity: similar vectors", "[embedder]") {
    Embedding a = {1.0f, 2.0f, 3.0f};
    Embedding b = {1.0f, 2.0f, 3.1f};
    double sim = cosine_similarity(a, b);
    REQUIRE(sim > 0.99);
    REQUIRE(sim <= 1.0);
}

TEST_CASE("cosine_similarity: empty vectors return 0", "[embedder]") {
    Embedding a = {};
    Embedding b = {};
    REQUIRE(cosine_similarity(a, b) == 0.0);
}

TEST_CASE("cosine_similarity: mismatched lengths return 0", "[embedder]") {
    Embedding a = {1.0f, 2.0f};
    Embedding b = {1.0f, 2.0f, 3.0f};
    REQUIRE(cosine_similarity(a, b) == 0.0);
}

TEST_CASE("cosine_similarity: zero vector returns 0", "[embedder]") {
    Embedding a = {0.0f, 0.0f, 0.0f};
    Embedding b = {1.0f, 2.0f, 3.0f};
    REQUIRE(cosine_similarity(a, b) == 0.0);
}

TEST_CASE("cosine_similarity: normalized vectors", "[embedder]") {
    // Two unit vectors at ~60 degree angle → cos(60°) ≈ 0.5
    Embedding a = {1.0f, 0.0f};
    Embedding b = {0.5f, 0.866025f}; // cos(60°), sin(60°)
    double sim = cosine_similarity(a, b);
    REQUIRE(std::abs(sim - 0.5) < 1e-4);
}

// ── Mock embedder ────────────────────────────────────────────

class MockEmbedder : public Embedder {
public:
    Embedding embed(const std::string& /*text*/) override {
        embed_count++;
        return test_embedding;
    }
    uint32_t dimensions() const override { return static_cast<uint32_t>(test_embedding.size()); }
    std::string embedder_name() const override { return "mock"; }

    Embedding test_embedding = {0.1f, 0.2f, 0.3f};
    int embed_count = 0;
};

TEST_CASE("MockEmbedder: basic interface", "[embedder]") {
    MockEmbedder embedder;
    auto result = embedder.embed("test text");
    REQUIRE(result.size() == 3);
    REQUIRE(embedder.embed_count == 1);
    REQUIRE(embedder.dimensions() == 3);
    REQUIRE(embedder.embedder_name() == "mock");
}
