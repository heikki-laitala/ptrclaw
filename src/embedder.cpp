#include "embedder.hpp"
#include "embedders/http_embedder.hpp"
#include "config.hpp"
#include "http.hpp"
#include <iostream>

namespace ptrclaw {

std::unique_ptr<Embedder> create_embedder(const Config& config, HttpClient& http) {
    const auto& emb = config.memory.embeddings;

    // Resolve OpenAI API key once (explicit embedding key, or provider fallback)
    std::string openai_key = emb.api_key;
    if (openai_key.empty()) openai_key = config.api_key_for("openai");

    // Resolve provider: explicit config, or auto-detect from available API keys
    std::string provider = emb.provider;
    if (provider.empty()) {
        if (!openai_key.empty()) {
            provider = "openai";
            std::cerr << "[embedder] Auto-detected OpenAI API key, enabling embeddings\n";
        }
    }
    if (provider.empty()) return nullptr;

    if (provider == "openai") {
        if (openai_key.empty()) {
            std::cerr << "[embedder] OpenAI embeddings configured but no API key found\n";
            return nullptr;
        }
        return create_openai_embedder(openai_key, http, emb.base_url, emb.model);
    }

    if (provider == "ollama") {
        return create_ollama_embedder(http, emb.base_url, emb.model);
    }

    std::cerr << "[embedder] Unknown embedding provider: " << provider << "\n";
    return nullptr;
}

} // namespace ptrclaw
