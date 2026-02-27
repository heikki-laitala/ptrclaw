#include "ollama_embedder.hpp"
#include <nlohmann/json.hpp>

namespace ptrclaw {

OllamaEmbedder::OllamaEmbedder(HttpClient& http, const std::string& base_url,
                                 const std::string& model)
    : http_(http)
    , base_url_(base_url.empty() ? "http://localhost:11434" : base_url)
    , model_(model.empty() ? "nomic-embed-text" : model)
{}

Embedding OllamaEmbedder::embed(const std::string& text) {
    nlohmann::json body = {
        {"model", model_},
        {"input", text}
    };

    std::vector<Header> headers = {
        {"Content-Type", "application/json"}
    };

    auto response = http_.post(base_url_ + "/api/embed", body.dump(), headers, 30);
    if (response.status_code != 200) {
        return {};
    }

    try {
        auto j = nlohmann::json::parse(response.body);
        auto& embeddings = j["embeddings"][0];
        Embedding result;
        result.reserve(embeddings.size());
        for (const auto& val : embeddings) {
            result.push_back(val.get<float>());
        }
        dimensions_ = static_cast<uint32_t>(result.size());
        return result;
    } catch (...) {
        return {};
    }
}

std::unique_ptr<Embedder> create_ollama_embedder(
    HttpClient& http, const std::string& base_url, const std::string& model) {
    return std::make_unique<OllamaEmbedder>(http, base_url, model);
}

} // namespace ptrclaw
