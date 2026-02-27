#include "http_embedder.hpp"
#include <nlohmann/json.hpp>

namespace ptrclaw {

HttpEmbedder::HttpEmbedder(Config config, HttpClient& http)
    : config_(std::move(config))
    , http_(http)
    , dimensions_(config_.default_dims)
{}

Embedding HttpEmbedder::embed(const std::string& text) {
    nlohmann::json body = {
        {"model", config_.model},
        {"input", text}
    };

    std::vector<Header> headers = {
        {"Content-Type", "application/json"}
    };
    if (!config_.api_key.empty()) {
        headers.push_back({"Authorization", "Bearer " + config_.api_key});
    }

    auto response = http_.post(
        config_.base_url + config_.endpoint, body.dump(), headers, 30);
    if (response.status_code != 200) {
        return {};
    }

    try {
        auto j = nlohmann::json::parse(response.body);
        auto& arr = j.at(nlohmann::json::json_pointer(config_.response_path));
        Embedding result;
        result.reserve(arr.size());
        for (const auto& val : arr) {
            result.push_back(val.get<float>());
        }
        dimensions_ = static_cast<uint32_t>(result.size());
        return result;
    } catch (...) {
        return {};
    }
}

std::unique_ptr<Embedder> create_openai_embedder(
    const std::string& api_key, HttpClient& http,
    const std::string& base_url, const std::string& model) {
    HttpEmbedder::Config cfg;
    cfg.name = "openai";
    cfg.api_key = api_key;
    cfg.base_url = base_url.empty() ? "https://api.openai.com/v1" : base_url;
    cfg.model = model.empty() ? "text-embedding-3-small" : model;
    cfg.endpoint = "/embeddings";
    cfg.response_path = "/data/0/embedding";
    cfg.default_dims = 1536;
    return std::make_unique<HttpEmbedder>(std::move(cfg), http);
}

std::unique_ptr<Embedder> create_ollama_embedder(
    HttpClient& http, const std::string& base_url, const std::string& model) {
    HttpEmbedder::Config cfg;
    cfg.name = "ollama";
    cfg.base_url = base_url.empty() ? "http://localhost:11434" : base_url;
    cfg.model = model.empty() ? "nomic-embed-text" : model;
    cfg.endpoint = "/api/embed";
    cfg.response_path = "/embeddings/0";
    cfg.default_dims = 768;
    return std::make_unique<HttpEmbedder>(std::move(cfg), http);
}

} // namespace ptrclaw
