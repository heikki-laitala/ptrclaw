#pragma once
#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include <cmath>

namespace ptrclaw {

using Embedding = std::vector<float>;

class HttpClient; // forward declare
struct Config;    // forward declare

// Abstract embedding provider interface
class Embedder {
public:
    virtual ~Embedder() = default;

    // Compute embedding vector for the given text
    virtual Embedding embed(const std::string& text) = 0;

    // Dimensionality of the embedding vectors
    virtual uint32_t dimensions() const = 0;

    // Human-readable name (e.g. "openai", "ollama")
    virtual std::string embedder_name() const = 0;
};

// Cosine similarity between two embedding vectors.
// Returns value in [-1, 1]. Assumes vectors are the same length.
// Returns 0.0 if either vector is empty or zero-magnitude.
inline double cosine_similarity(const Embedding& a, const Embedding& b) {
    if (a.empty() || b.empty() || a.size() != b.size()) return 0.0;

    double dot = 0.0;
    double norm_a = 0.0;
    double norm_b = 0.0;

    for (size_t i = 0; i < a.size(); ++i) {
        dot += static_cast<double>(a[i]) * static_cast<double>(b[i]);
        norm_a += static_cast<double>(a[i]) * static_cast<double>(a[i]);
        norm_b += static_cast<double>(b[i]) * static_cast<double>(b[i]);
    }

    double denom = std::sqrt(norm_a) * std::sqrt(norm_b);
    if (denom < 1e-12) return 0.0;

    return dot / denom;
}

// Compute hybrid score from a normalized text score [0,1] and
// a cosine similarity [-1,1].  Returns weighted combination in [0,1].
// When only one signal is available (the other is 0), that signal
// is used unweighted so results still rank meaningfully.
inline double hybrid_score(double text_norm, double cosine_sim,
                           double text_weight, double vector_weight,
                           bool has_text, bool has_vector) {
    double vec_norm = (cosine_sim + 1.0) / 2.0; // shift [-1,1] to [0,1]
    if (has_text && has_vector) return text_weight * text_norm + vector_weight * vec_norm;
    if (has_text) return text_norm;
    if (has_vector) return vec_norm;
    return 0.0;
}

// Create an embedder from config. Returns nullptr if embeddings are disabled
// or the configured provider is not recognized.
std::unique_ptr<Embedder> create_embedder(const Config& config, HttpClient& http);

} // namespace ptrclaw
