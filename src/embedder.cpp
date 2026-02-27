#include "embedder.hpp"
#include "embedders/http_embedder.hpp"
#include "config.hpp"
#include "http.hpp"

namespace ptrclaw {

std::unique_ptr<Embedder> create_embedder(const Config& config, HttpClient& http) {
    const auto& emb = config.memory.embeddings;
    if (emb.provider.empty()) return nullptr;

    if (emb.provider == "openai") {
        // Fall back to providers.openai.api_key if embedding-specific key not set
        std::string api_key = emb.api_key;
        if (api_key.empty()) {
            api_key = config.api_key_for("openai");
        }
        if (api_key.empty()) return nullptr;

        return create_openai_embedder(api_key, http, emb.base_url, emb.model);
    }

    if (emb.provider == "ollama") {
        return create_ollama_embedder(http, emb.base_url, emb.model);
    }

    return nullptr;
}

} // namespace ptrclaw
