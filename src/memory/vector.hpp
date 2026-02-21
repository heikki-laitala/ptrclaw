#pragma once
#include <vector>
#include <string>
#include <cstdint>

namespace ptrclaw {

struct ScoredResult {
    std::string key;
    double score;
};

// Cosine similarity between two float vectors. Returns 0.0 if either is empty or different lengths.
double cosine_similarity(const std::vector<float>& a, const std::vector<float>& b);

// Serialize a float vector to a binary string (for DB storage).
std::string serialize_vector(const std::vector<float>& vec);

// Deserialize a binary string back to a float vector.
std::vector<float> deserialize_vector(const std::string& data);

// Merge keyword-scored and vector-scored results with configurable weights.
// Returns merged results sorted by combined score, deduplicated by key.
std::vector<ScoredResult> hybrid_merge(const std::vector<ScoredResult>& keyword_results,
                                       const std::vector<ScoredResult>& vector_results,
                                       double keyword_weight, double vector_weight,
                                       uint32_t limit);

} // namespace ptrclaw
