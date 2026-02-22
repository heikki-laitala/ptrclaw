#pragma once
#include <string>
#include <cstdint>
#include <unordered_map>
#include <nlohmann/json.hpp>

namespace ptrclaw {

struct ProviderEntry {
    std::string api_key;
    std::string base_url;
};

struct AgentConfig {
    uint32_t max_tool_iterations = 10;
    uint32_t max_history_messages = 50;
    uint32_t token_limit = 128000;
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
};

struct Config {
    std::string provider = "anthropic";
    std::string model = "claude-sonnet-4-20250514";
    double temperature = 0.7;
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

    // Get JSON config for a channel name (empty object if absent)
    nlohmann::json channel_config(const std::string& name) const;
};

} // namespace ptrclaw
