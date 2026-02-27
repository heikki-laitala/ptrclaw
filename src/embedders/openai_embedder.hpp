#pragma once
#include "../embedder.hpp"
#include "../http.hpp"
#include <string>

namespace ptrclaw {

class OpenAIEmbedder : public Embedder {
public:
    OpenAIEmbedder(const std::string& api_key, HttpClient& http,
                   const std::string& base_url, const std::string& model);

    Embedding embed(const std::string& text) override;
    uint32_t dimensions() const override { return dimensions_; }
    std::string embedder_name() const override { return "openai"; }

private:
    std::string api_key_;
    HttpClient& http_;
    std::string base_url_;
    std::string model_;
    uint32_t dimensions_ = 1536; // text-embedding-3-small default
};

std::unique_ptr<Embedder> create_openai_embedder(
    const std::string& api_key, HttpClient& http,
    const std::string& base_url, const std::string& model);

} // namespace ptrclaw
