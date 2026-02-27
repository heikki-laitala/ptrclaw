#include "openai_embedder.hpp"
#include <nlohmann/json.hpp>

namespace ptrclaw {

OpenAIEmbedder::OpenAIEmbedder(const std::string& api_key, HttpClient& http,
                                 const std::string& base_url, const std::string& model)
    : api_key_(api_key)
    , http_(http)
    , base_url_(base_url.empty() ? "https://api.openai.com/v1" : base_url)
    , model_(model.empty() ? "text-embedding-3-small" : model)
{}

Embedding OpenAIEmbedder::embed(const std::string& text) {
    nlohmann::json body = {
        {"model", model_},
        {"input", text}
    };

    std::vector<Header> headers = {
        {"Content-Type", "application/json"},
        {"Authorization", "Bearer " + api_key_}
    };

    auto response = http_.post(base_url_ + "/embeddings", body.dump(), headers, 30);
    if (response.status_code != 200) {
        return {};
    }

    try {
        auto j = nlohmann::json::parse(response.body);
        auto& data = j["data"][0]["embedding"];
        Embedding result;
        result.reserve(data.size());
        for (const auto& val : data) {
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
    return std::make_unique<OpenAIEmbedder>(api_key, http, base_url, model);
}

} // namespace ptrclaw
