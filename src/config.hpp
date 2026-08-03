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
    // OpenAI's `user` request field: a stable, opaque identifier for the end user or
    // tenant this process speaks for. OpenAI documents it for abuse signals; a gateway in
    // front of one can also use it to attribute and budget spend, which is not possible if
    // every request looks identical. Empty means the field is omitted entirely, so a
    // provider that has never heard of it sees no change.
    std::string user;
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
    uint32_t max_tool_iterations = 50;
    uint32_t max_history_messages = 50;
    uint32_t token_limit = 128000;
    bool disable_streaming = false;
    std::string tee_mode = "off";  // "off", "failures", "always"
    uint32_t tool_timeout = 120;   // seconds, 0 = no timeout
    // How long a session may sit idle before it is dropped. Was hard-coded to an hour at
    // the eviction call site, which suits a long-lived assistant and not a deployment
    // that runs one process per agent: there, an hour of retention after the last visitor
    // message is memory paid for nothing. Also the only way to observe reclamation in
    // less than an hour, which makes per-session memory measurable at all.
    uint32_t session_max_idle_seconds = 3600;
};

struct EmbeddingConfig {
    std::string provider;       // "openai", "ollama", "" (disabled)
    std::string model;          // model name (empty = provider default)
    std::string base_url;       // override (empty = provider default)
    std::string api_key;        // for OpenAI (empty = use providers.openai.api_key)
    double text_weight = 0.4;   // hybrid search text score weight
    double vector_weight = 0.6; // hybrid search vector score weight
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
    uint32_t recency_half_life = 0;    // 0 = disabled, else seconds for half-life decay
    uint32_t knowledge_max_idle_days = 30;  // 0 = disabled, else days of inactivity before purge
    double knowledge_survival_chance = 0.05; // [0.0, 1.0] random survival probability
    EmbeddingConfig embeddings;         // vector search config (disabled by default)
};

struct Config {
    std::string provider = "anthropic";
    std::string model = "claude-sonnet-4-6";
    double temperature = 0.7;
    bool dev = false;      // Enables developer-only commands (e.g. /soul)
    // Whether slash commands are dispatched for messages arriving over a channel.
    //
    // Off by default, and the default is the point. The command surface is the
    // operator's: /model and /provider change what the model is, /memory import writes
    // memory entries — including soul:identity — and /start clears history and restarts
    // the hatching interview. On a CLI that is exactly right; on a channel it means
    // whoever can send a message can do all of it, unauthenticated, because a channel
    // has no notion of who is speaking.
    //
    // A deployment that puts an agent in front of the public therefore gets the safe
    // behaviour by doing nothing, and every future channel inherits it. Turn it on for
    // a personal bot, where the only person messaging it is the operator.
    bool allow_channel_commands = false;
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
