#pragma once
#include <string>
#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>
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
    // Which models the subscription tokens may serve. Empty means the built-in set
    // (the codex and gpt-5 families); "*" matches any model. The entitlements behind a
    // subscription are not discoverable from here and change over time, so an account
    // that can reach more — or fewer — models than the default set says so here rather
    // than waiting for a release. See openai_oauth_eligible().
    std::vector<std::string> oauth_models;
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
    // "shared" — every session reads and writes one store (the default, and the
    // behaviour before this key existed). "session" — each session gets its own
    // store file under <dir of path>/sessions/, so keys, recall and links never
    // cross between sessions.
    std::string isolation = "shared";
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

// Filesystem scope for a pod serving many sessions at once.
//
// Both roots are empty by default, which is what keeps the personal agent unchanged: with
// no roots configured nothing is scoped and no tool behaves differently. They only matter
// in a build that compiles the workspace-scoped tools (with_serving).
struct ServingConfig {
    // One directory every session may read and none may write. Where an external context
    // manager stages the files all sessions work from. Empty means there is no shared
    // context and a session can reach nothing but its own workspace.
    std::string context_dir;
    // Parent of the per-session workspaces, each derived with session_store_path() so the
    // layout matches the per-session memory stores. A session reads and writes its own.
    std::string workspace_root;
    // Whether a request may omit the session id and have one generated. Off by default:
    // an id is a routing key the caller has always had to supply, and a channel that
    // silently invents one hides a client bug.
    bool generate_session_ids = false;
};

// Upper bound on Config::workers.
constexpr uint32_t kMaxWorkers = 64;

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

    // Worker threads running agent turns. 1 = turns run inline on the poll loop,
    // one at a time for the whole process. Above 1, turns for different sessions
    // run in parallel; turns for one session stay serialised. Channel modes only.
    uint32_t workers = 1;

    std::unordered_map<std::string, ProviderEntry> providers;

    AgentConfig agent;
    std::unordered_map<std::string, nlohmann::json> channels;
    MemoryConfig memory;
    ServingConfig serving;

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

// Guards the OAuth fields of Config::providers, the one part of Config that
// changes while turns are running.
//
// OpenAIProvider rotates its tokens mid-turn and calls back to write them to the
// shared Config and to ~/.ptrclaw/config.json. With `workers > 1` that callback
// fires on whichever worker hit the expiry, while another worker may be reading
// the same entry to build a provider for a new session — two turns expiring at
// once would race on the strings and on the config file's temp path, losing a
// rotated refresh token and locking the deployment out of OpenAI.
//
// Held across the file write too, so the in-memory entry and the file cannot
// disagree. The refresh is an HTTP round trip away, so contention is nil.
//
// Not taken by Config's own accessors: api_key_for() and base_url_for() read
// fields the refresh never writes, and locking there would deadlock the
// composite operations below, which call them while holding it.
std::mutex& provider_credentials_mutex();

// Serialises the OAuth refresh itself, so only one of a process's providers
// performs the token round trip at a time and the others adopt its result
// instead of re-spending a refresh token that has already been rotated.
//
// Lock order: this one first, then provider_credentials_mutex(), which the
// reload and persist callbacks take from inside a refresh. Nothing takes them
// the other way round.
std::mutex& oauth_refresh_mutex();

} // namespace ptrclaw
