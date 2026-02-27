#pragma once
#include "../embedder.hpp"
#include "../http.hpp"
#include <string>

namespace ptrclaw {

class OllamaEmbedder : public Embedder {
public:
    OllamaEmbedder(HttpClient& http, const std::string& base_url,
                    const std::string& model);

    Embedding embed(const std::string& text) override;
    uint32_t dimensions() const override { return dimensions_; }
    std::string embedder_name() const override { return "ollama"; }

private:
    HttpClient& http_;
    std::string base_url_;
    std::string model_;
    uint32_t dimensions_ = 768; // nomic-embed-text default
};

std::unique_ptr<Embedder> create_ollama_embedder(
    HttpClient& http, const std::string& base_url, const std::string& model);

} // namespace ptrclaw
