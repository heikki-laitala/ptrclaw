#pragma once
#include "../embedder.hpp"
#include "../http.hpp"
#include <string>

namespace ptrclaw {

// Unified HTTP-based embedder. Supports OpenAI-compatible and Ollama APIs
// by parameterizing the endpoint, auth, and response JSON path.
class HttpEmbedder : public Embedder {
public:
    struct Config {
        std::string name;           // e.g. "openai", "ollama"
        std::string api_key;        // empty = no Authorization header
        std::string base_url;       // e.g. "https://api.openai.com/v1"
        std::string model;          // e.g. "text-embedding-3-small"
        std::string endpoint;       // URL path, e.g. "/embeddings"
        std::string response_path;  // JSON pointer to float array, e.g. "/data/0/embedding"
        uint32_t default_dims;      // fallback until first response
    };

    HttpEmbedder(Config config, HttpClient& http);

    Embedding embed(const std::string& text) override;
    uint32_t dimensions() const override { return dimensions_; }
    std::string embedder_name() const override { return config_.name; }

private:
    Config config_;
    HttpClient& http_;
    uint32_t dimensions_;
};

std::unique_ptr<Embedder> create_openai_embedder(
    const std::string& api_key, HttpClient& http,
    const std::string& base_url, const std::string& model);

std::unique_ptr<Embedder> create_ollama_embedder(
    HttpClient& http, const std::string& base_url, const std::string& model);

} // namespace ptrclaw
