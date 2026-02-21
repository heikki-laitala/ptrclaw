#include "vector.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace ptrclaw {

double cosine_similarity(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.empty() || b.empty() || a.size() != b.size()) return 0.0;

    double dot = 0.0;
    double norm_a = 0.0;
    double norm_b = 0.0;

    for (size_t i = 0; i < a.size(); i++) {
        dot    += static_cast<double>(a[i]) * static_cast<double>(b[i]);
        norm_a += static_cast<double>(a[i]) * static_cast<double>(a[i]);
        norm_b += static_cast<double>(b[i]) * static_cast<double>(b[i]);
    }

    norm_a = std::sqrt(norm_a);
    norm_b = std::sqrt(norm_b);

    if (norm_a == 0.0 || norm_b == 0.0) return 0.0;

    return dot / (norm_a * norm_b);
}

std::string serialize_vector(const std::vector<float>& vec) {
    if (vec.empty()) return {};

    std::string data(sizeof(float) * vec.size(), '\0');
    std::memcpy(data.data(), vec.data(), sizeof(float) * vec.size());
    return data;
}

std::vector<float> deserialize_vector(const std::string& data) {
    if (data.empty() || data.size() % sizeof(float) != 0) return {};

    std::vector<float> vec(data.size() / sizeof(float));
    std::memcpy(vec.data(), data.data(), data.size());
    return vec;
}

std::vector<ScoredResult> hybrid_merge(const std::vector<ScoredResult>& keyword_results,
                                       const std::vector<ScoredResult>& vector_results,
                                       double keyword_weight, double vector_weight,
                                       uint32_t limit) {
    std::unordered_map<std::string, double> scores;

    for (const auto& r : keyword_results) {
        scores[r.key] += r.score * keyword_weight;
    }

    for (const auto& r : vector_results) {
        scores[r.key] += r.score * vector_weight;
    }

    std::vector<ScoredResult> merged;
    merged.reserve(scores.size());
    for (auto& [key, score] : scores) {
        merged.push_back({key, score});
    }

    std::sort(merged.begin(), merged.end(),
              [](const ScoredResult& a, const ScoredResult& b) {
                  return a.score > b.score;
              });

    if (merged.size() > limit) {
        merged.resize(limit);
    }

    return merged;
}

} // namespace ptrclaw
