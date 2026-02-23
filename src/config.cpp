#include "config.hpp"
#include "util.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>

namespace ptrclaw {

nlohmann::json Config::defaults_json() {
    return {
        {"provider", "anthropic"},
        {"model", "claude-sonnet-4-20250514"},
        {"temperature", 0.7},
        {"dev", false},
        {"base_url", ""},
        {"providers", {
            {"anthropic", {{"api_key", ""}}},
            {"openai", {{"api_key", ""}}},
            {"openrouter", {{"api_key", ""}}},
            {"ollama", {{"base_url", "http://localhost:11434"}}},
            {"compatible", {{"base_url", ""}}}
        }},
        {"agent", {
            {"max_tool_iterations", 10},
            {"max_history_messages", 50},
            {"token_limit", 128000}
        }},
        {"channels", {
            {"telegram", {{"bot_token", ""}, {"allow_from", nlohmann::json::array()}, {"reply_in_private", true}, {"proxy", ""}}},
            {"whatsapp", {{"access_token", ""}, {"phone_number_id", ""}, {"verify_token", ""}, {"app_secret", ""}, {"allow_from", nlohmann::json::array()}, {"webhook_listen", ""}, {"webhook_secret", ""}, {"webhook_max_body", 65536}}}
        }},
        {"memory", {
#ifdef PTRCLAW_HAS_SQLITE_MEMORY
            {"backend", "sqlite"},
#else
            {"backend", "json"},
#endif
            {"auto_save", false},
            {"recall_limit", 5},
            {"hygiene_max_age", 604800},
            {"response_cache", false},
            {"cache_ttl", 3600},
            {"cache_max_entries", 100},
            {"enrich_depth", 1},
            {"synthesis", true},
            {"synthesis_interval", 5}
        }}
    };
}

static nlohmann::json merge_defaults(const nlohmann::json& existing,
                                      const nlohmann::json& defaults) {
    nlohmann::json merged = existing;
    for (auto& [key, value] : defaults.items()) {
        if (!merged.contains(key)) {
            merged[key] = value;
        } else if (value.is_object() && merged[key].is_object()) {
            merged[key] = merge_defaults(merged[key], value);
        }
    }
    return merged;
}

Config Config::load() {
    Config cfg;

    std::string config_path = expand_home("~/.ptrclaw/config.json");
    nlohmann::json j;

    std::ifstream file(config_path);
    if (file.is_open()) {
        try {
            nlohmann::json original = nlohmann::json::parse(file);
            file.close();
            j = merge_defaults(original, defaults_json());
            if (j != original) {
                atomic_write_file(config_path, j.dump(4) + "\n");
                std::cerr << "[config] Migrated config with new defaults: "
                          << config_path << "\n";
            }
        } catch (...) {
            // Config file is malformed — fall back to defaults
            j = defaults_json();
        }
    } else {
        j = defaults_json();
        atomic_write_file(config_path, j.dump(4) + "\n");
        std::cerr << "[config] Created default config: " << config_path << "\n";
    }

    // Parse JSON into Config struct
    if (j.contains("provider") && j["provider"].is_string())
        cfg.provider = j["provider"].get<std::string>();
    if (j.contains("model") && j["model"].is_string())
        cfg.model = j["model"].get<std::string>();
    if (j.contains("temperature") && j["temperature"].is_number())
        cfg.temperature = j["temperature"].get<double>();
    if (j.contains("dev") && j["dev"].is_boolean())
        cfg.dev = j["dev"].get<bool>();

    if (j.contains("base_url") && j["base_url"].is_string())
        cfg.base_url = j["base_url"].get<std::string>();

    if (j.contains("providers") && j["providers"].is_object()) {
        for (auto& [name, obj] : j["providers"].items()) {
            if (!obj.is_object()) continue;
            ProviderEntry entry;
            if (obj.contains("api_key") && obj["api_key"].is_string())
                entry.api_key = obj["api_key"].get<std::string>();
            if (obj.contains("base_url") && obj["base_url"].is_string())
                entry.base_url = obj["base_url"].get<std::string>();
            cfg.providers[name] = std::move(entry);
        }
    }

    if (j.contains("agent") && j["agent"].is_object()) {
        auto& a = j["agent"];
        if (a.contains("max_tool_iterations") && a["max_tool_iterations"].is_number_unsigned())
            cfg.agent.max_tool_iterations = a["max_tool_iterations"].get<uint32_t>();
        if (a.contains("max_history_messages") && a["max_history_messages"].is_number_unsigned())
            cfg.agent.max_history_messages = a["max_history_messages"].get<uint32_t>();
        if (a.contains("token_limit") && a["token_limit"].is_number_unsigned())
            cfg.agent.token_limit = a["token_limit"].get<uint32_t>();
    }

    // Channel configurations — store raw JSON per channel name
    if (j.contains("channels") && j["channels"].is_object()) {
        for (auto& [name, obj] : j["channels"].items()) {
            if (obj.is_object())
                cfg.channels[name] = obj;
        }
    }

    // Memory configuration
    if (j.contains("memory") && j["memory"].is_object()) {
        auto& m = j["memory"];
        if (m.contains("backend") && m["backend"].is_string())
            cfg.memory.backend = m["backend"].get<std::string>();
        if (m.contains("path") && m["path"].is_string())
            cfg.memory.path = m["path"].get<std::string>();
        if (m.contains("auto_save") && m["auto_save"].is_boolean())
            cfg.memory.auto_save = m["auto_save"].get<bool>();
        if (m.contains("recall_limit") && m["recall_limit"].is_number_unsigned())
            cfg.memory.recall_limit = m["recall_limit"].get<uint32_t>();
        if (m.contains("hygiene_max_age") && m["hygiene_max_age"].is_number_unsigned())
            cfg.memory.hygiene_max_age = m["hygiene_max_age"].get<uint32_t>();
        if (m.contains("response_cache") && m["response_cache"].is_boolean())
            cfg.memory.response_cache = m["response_cache"].get<bool>();
        if (m.contains("cache_ttl") && m["cache_ttl"].is_number_unsigned())
            cfg.memory.cache_ttl = m["cache_ttl"].get<uint32_t>();
        if (m.contains("cache_max_entries") && m["cache_max_entries"].is_number_unsigned())
            cfg.memory.cache_max_entries = m["cache_max_entries"].get<uint32_t>();
        if (m.contains("enrich_depth") && m["enrich_depth"].is_number_unsigned())
            cfg.memory.enrich_depth = m["enrich_depth"].get<uint32_t>();
        if (m.contains("synthesis") && m["synthesis"].is_boolean())
            cfg.memory.synthesis = m["synthesis"].get<bool>();
        if (m.contains("synthesis_interval") && m["synthesis_interval"].is_number_unsigned())
            cfg.memory.synthesis_interval = m["synthesis_interval"].get<uint32_t>();
    }

    // Environment variables always override config file
    if (const char* v = std::getenv("ANTHROPIC_API_KEY"))
        cfg.providers["anthropic"].api_key = v;
    if (const char* v = std::getenv("OPENAI_API_KEY"))
        cfg.providers["openai"].api_key = v;
    if (const char* v = std::getenv("OPENROUTER_API_KEY"))
        cfg.providers["openrouter"].api_key = v;
    if (const char* v = std::getenv("BASE_URL"))
        cfg.base_url = v;
    if (const char* v = std::getenv("OLLAMA_BASE_URL"))
        cfg.providers["ollama"].base_url = v;
    if (const char* v = std::getenv("COMPATIBLE_BASE_URL"))
        cfg.providers["compatible"].base_url = v;

    // Channel env var overrides
    if (const char* v = std::getenv("TELEGRAM_BOT_TOKEN"))
        cfg.channels["telegram"]["bot_token"] = v;
    if (const char* v = std::getenv("WHATSAPP_ACCESS_TOKEN"))
        cfg.channels["whatsapp"]["access_token"] = v;
    if (const char* v = std::getenv("WHATSAPP_PHONE_ID"))
        cfg.channels["whatsapp"]["phone_number_id"] = v;
    if (const char* v = std::getenv("WHATSAPP_VERIFY_TOKEN"))
        cfg.channels["whatsapp"]["verify_token"] = v;
    if (const char* v = std::getenv("WHATSAPP_WEBHOOK_LISTEN"))
        cfg.channels["whatsapp"]["webhook_listen"] = v;
    if (const char* v = std::getenv("WHATSAPP_WEBHOOK_SECRET"))
        cfg.channels["whatsapp"]["webhook_secret"] = v;

    return cfg;
}

std::string Config::api_key_for(const std::string& prov) const {
    auto it = providers.find(prov);
    if (it != providers.end()) return it->second.api_key;
    return {};
}

std::string Config::base_url_for(const std::string& prov) const {
    if (!base_url.empty()) return base_url;
    auto it = providers.find(prov);
    if (it != providers.end()) return it->second.base_url;
    return {};
}

nlohmann::json Config::channel_config(const std::string& name) const {
    auto it = channels.find(name);
    if (it != channels.end()) return it->second;
    return nlohmann::json::object();
}

} // namespace ptrclaw
