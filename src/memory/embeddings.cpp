#include "embeddings.hpp"
#include "../config.hpp"
#include "../http.hpp"
#include <nlohmann/json.hpp>
#include <functional>

using json = nlohmann::json;

namespace ptrclaw {

OpenAiEmbedding::OpenAiEmbedding(HttpClient& http, const std::string& api_key,
                                   const std::string& model, uint32_t dimensions,
                                   const std::string& base_url)
    : http_(http), api_key_(api_key), model_(model), dimensions_(dimensions),
      base_url_(base_url) {}

std::vector<float> OpenAiEmbedding::embed(const std::string& text) {
    size_t key = std::hash<std::string>{}(text);

    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        auto it = cache_.find(key);
        if (it != cache_.end()) {
            return it->second;
        }
    }

    json body;
    body["input"] = text;
    body["model"] = model_;
    body["dimensions"] = dimensions_;

    std::vector<Header> headers = {
        {"Authorization", "Bearer " + api_key_},
        {"Content-Type", "application/json"}
    };

    std::string url = base_url_ + "/v1/embeddings";

    HttpResponse response;
    try {
        response = http_.post(url, body.dump(), headers);
    } catch (...) {
        return {};
    }

    if (response.status_code < 200 || response.status_code >= 300) {
        return {};
    }

    try {
        auto resp = json::parse(response.body);
        auto embedding_json = resp["data"][0]["embedding"];
        std::vector<float> embedding;
        embedding.reserve(embedding_json.size());
        for (const auto& val : embedding_json) {
            embedding.push_back(val.get<float>());
        }

        std::lock_guard<std::mutex> lock(cache_mutex_);
        cache_[key] = embedding;
        return embedding;
    } catch (...) {
        return {};
    }
}

std::unique_ptr<EmbeddingProvider> create_embedding_provider(
    const Config& config, HttpClient& http) {

    const auto& emb = config.memory.embeddings;

    if (emb.provider == "none" || emb.api_key.empty()) {
        return std::make_unique<NoopEmbedding>();
    }

    std::string base_url = "https://api.openai.com";
    if (emb.provider.rfind("custom:", 0) == 0) {
        base_url = emb.provider.substr(7); // everything after "custom:"
    }

    return std::make_unique<OpenAiEmbedding>(http, emb.api_key, emb.model,
                                              emb.dimensions, base_url);
}

} // namespace ptrclaw
