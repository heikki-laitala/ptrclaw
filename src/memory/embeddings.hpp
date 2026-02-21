#pragma once
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <mutex>

namespace ptrclaw {

class HttpClient; // forward declare

// Abstract embedding provider
class EmbeddingProvider {
public:
    virtual ~EmbeddingProvider() = default;
    virtual std::vector<float> embed(const std::string& text) = 0;
    virtual uint32_t dimensions() const = 0;
    virtual std::string name() const = 0;
};

// No-op embedding provider (keyword-only fallback)
class NoopEmbedding : public EmbeddingProvider {
public:
    std::vector<float> embed(const std::string&) override { return {}; }
    uint32_t dimensions() const override { return 0; }
    std::string name() const override { return "none"; }
};

// OpenAI-compatible embedding provider
class OpenAiEmbedding : public EmbeddingProvider {
public:
    OpenAiEmbedding(HttpClient& http, const std::string& api_key,
                    const std::string& model, uint32_t dimensions,
                    const std::string& base_url = "https://api.openai.com");

    std::vector<float> embed(const std::string& text) override;
    uint32_t dimensions() const override { return dimensions_; }
    std::string name() const override { return "openai"; }

private:
    HttpClient& http_;
    std::string api_key_;
    std::string model_;
    uint32_t dimensions_;
    std::string base_url_;

    // Simple cache: hash(text) -> embedding
    std::unordered_map<size_t, std::vector<float>> cache_;
    std::mutex cache_mutex_;
};

struct Config;

// Create an embedding provider based on config
std::unique_ptr<EmbeddingProvider> create_embedding_provider(
    const Config& config, HttpClient& http);

} // namespace ptrclaw
