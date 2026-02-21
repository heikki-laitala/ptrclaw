#pragma once
#include <string>
#include <vector>
#include <optional>
#include <cstdint>

namespace ptrclaw {

struct AgentConfig {
    uint32_t max_tool_iterations = 10;
    uint32_t max_history_messages = 50;
    uint32_t token_limit = 128000;
};

struct TelegramChannelConfig {
    std::string bot_token;
    std::vector<std::string> allow_from;
    bool reply_in_private = true;
    std::string proxy;
};

struct WhatsAppChannelConfig {
    std::string access_token;
    std::string phone_number_id;
    std::string verify_token;
    std::string app_secret;
    std::vector<std::string> allow_from;
};

struct ChannelsConfig {
    std::optional<TelegramChannelConfig> telegram;
    std::optional<WhatsAppChannelConfig> whatsapp;
};

struct Config {
    // API keys (from config or env vars)
    std::string anthropic_api_key;
    std::string openai_api_key;
    std::string openrouter_api_key;
    std::string ollama_base_url = "http://localhost:11434";
    std::string compatible_base_url;

    // Defaults
    std::string default_provider = "anthropic";
    std::string default_model = "claude-sonnet-4-20250514";
    double default_temperature = 0.7;

    AgentConfig agent;
    ChannelsConfig channels;

    // Load from ~/.ptrclaw/config.json + env vars
    static Config load();

    // Get API key for a provider name
    std::string api_key_for(const std::string& provider) const;

    // Get base URL for a provider name (empty = use provider default)
    std::string base_url_for(const std::string& provider) const;
};

} // namespace ptrclaw
