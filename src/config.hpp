#pragma once
#include <string>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <nlohmann/json.hpp>

namespace ptrclaw {

struct ProviderEntry {
    std::string api_key;
    std::string base_url;
    bool prompt_caching = false; // Anthropic-only, provider-side prompt caching

    // OpenAI subscription OAuth (Codex) support
    bool use_oauth = false;
    std::string oauth_access_token;
    std::string oauth_refresh_token;
    uint64_t oauth_expires_at = 0; // epoch seconds
    std::string oauth_client_id;
    std::string oauth_token_url;
};

struct AgentConfig {
    uint32_t max_tool_iterations = 10;
    uint32_t max_history_messages = 50;
    uint32_t token_limit = 128000;
    bool disable_streaming = false;
};

struct MemoryConfig {
#ifdef PTRCLAW_HAS_SQLITE_MEMORY
    std::string backend = "sqlite";
#else
    std::string backend = "json";
#endif
    std::string path;
    bool auto_save = false;
    uint32_t recall_limit = 5;
    uint32_t hygiene_max_age = 604800;  // 7 days
    bool response_cache = false;
    uint32_t cache_ttl = 3600;
    uint32_t cache_max_entries = 100;
    uint32_t enrich_depth = 1;          // 0 = flat, 1 = follow links
    bool synthesis = true;
    uint32_t synthesis_interval = 5;    // synthesize every N user messages
    bool sqlite_trusted_schema = false; // security: keep OFF unless explicitly needed
};

struct Config {
    std::string provider = "anthropic";
    std::string model = "claude-sonnet-4-6";
    double temperature = 0.7;
    bool dev = false;      // Enables developer-only commands (e.g. /soul)
    std::string base_url;  // Global override — applies to the active provider

    std::unordered_map<std::string, ProviderEntry> providers;

    AgentConfig agent;
    std::unordered_map<std::string, nlohmann::json> channels;
    MemoryConfig memory;

    // Load from ~/.ptrclaw/config.json + env vars
    static Config load();

    // Default config JSON (used by load() and tests)
    static nlohmann::json defaults_json();

    // Get API key for a provider name
    std::string api_key_for(const std::string& provider) const;

    // Get base URL for a provider name (empty = use provider default)
    std::string base_url_for(const std::string& provider) const;

    // Provider-specific prompt caching toggle (currently Anthropic)
    bool prompt_caching_for(const std::string& provider) const;

    // Get JSON config for a channel name (empty object if absent)
    nlohmann::json channel_config(const std::string& name) const;

    // Persist provider + model selection to config file
    bool persist_selection() const;
};

// Read-modify-write ~/.ptrclaw/config.json atomically.
// The callback receives a mutable reference to the parsed JSON.
bool modify_config_json(const std::function<void(nlohmann::json&)>& modifier);

} // namespace ptrclaw
