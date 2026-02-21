#include "config.hpp"
#include "util.hpp"

#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>

namespace ptrclaw {

Config Config::load() {
    Config cfg;

    std::string config_path = expand_home("~/.ptrclaw/config.json");
    std::ifstream file(config_path);
    if (file.is_open()) {
        try {
            nlohmann::json j = nlohmann::json::parse(file);

            if (j.contains("anthropic_api_key") && j["anthropic_api_key"].is_string())
                cfg.anthropic_api_key = j["anthropic_api_key"].get<std::string>();
            if (j.contains("openai_api_key") && j["openai_api_key"].is_string())
                cfg.openai_api_key = j["openai_api_key"].get<std::string>();
            if (j.contains("openrouter_api_key") && j["openrouter_api_key"].is_string())
                cfg.openrouter_api_key = j["openrouter_api_key"].get<std::string>();
            if (j.contains("ollama_base_url") && j["ollama_base_url"].is_string())
                cfg.ollama_base_url = j["ollama_base_url"].get<std::string>();
            if (j.contains("compatible_base_url") && j["compatible_base_url"].is_string())
                cfg.compatible_base_url = j["compatible_base_url"].get<std::string>();

            if (j.contains("default_provider") && j["default_provider"].is_string())
                cfg.default_provider = j["default_provider"].get<std::string>();
            if (j.contains("default_model") && j["default_model"].is_string())
                cfg.default_model = j["default_model"].get<std::string>();
            if (j.contains("default_temperature") && j["default_temperature"].is_number())
                cfg.default_temperature = j["default_temperature"].get<double>();

            if (j.contains("agent") && j["agent"].is_object()) {
                auto& a = j["agent"];
                if (a.contains("max_tool_iterations") && a["max_tool_iterations"].is_number_unsigned())
                    cfg.agent.max_tool_iterations = a["max_tool_iterations"].get<uint32_t>();
                if (a.contains("max_history_messages") && a["max_history_messages"].is_number_unsigned())
                    cfg.agent.max_history_messages = a["max_history_messages"].get<uint32_t>();
                if (a.contains("token_limit") && a["token_limit"].is_number_unsigned())
                    cfg.agent.token_limit = a["token_limit"].get<uint32_t>();
            }

            // Channel configurations
            if (j.contains("channels") && j["channels"].is_object()) {
                auto& ch = j["channels"];

                if (ch.contains("telegram") && ch["telegram"].is_object()) {
                    auto& t = ch["telegram"];
                    TelegramChannelConfig tc;
                    if (t.contains("bot_token") && t["bot_token"].is_string())
                        tc.bot_token = t["bot_token"].get<std::string>();
                    if (t.contains("reply_in_private") && t["reply_in_private"].is_boolean())
                        tc.reply_in_private = t["reply_in_private"].get<bool>();
                    if (t.contains("proxy") && t["proxy"].is_string())
                        tc.proxy = t["proxy"].get<std::string>();
                    if (t.contains("allow_from") && t["allow_from"].is_array()) {
                        for (const auto& u : t["allow_from"]) {
                            if (u.is_string()) tc.allow_from.push_back(u.get<std::string>());
                        }
                    }
                    if (!tc.bot_token.empty())
                        cfg.channels.telegram = std::move(tc);
                }

                if (ch.contains("whatsapp") && ch["whatsapp"].is_object()) {
                    auto& w = ch["whatsapp"];
                    WhatsAppChannelConfig wc;
                    if (w.contains("access_token") && w["access_token"].is_string())
                        wc.access_token = w["access_token"].get<std::string>();
                    if (w.contains("phone_number_id") && w["phone_number_id"].is_string())
                        wc.phone_number_id = w["phone_number_id"].get<std::string>();
                    if (w.contains("verify_token") && w["verify_token"].is_string())
                        wc.verify_token = w["verify_token"].get<std::string>();
                    if (w.contains("app_secret") && w["app_secret"].is_string())
                        wc.app_secret = w["app_secret"].get<std::string>();
                    if (w.contains("allow_from") && w["allow_from"].is_array()) {
                        for (const auto& p : w["allow_from"]) {
                            if (p.is_string()) wc.allow_from.push_back(p.get<std::string>());
                        }
                    }
                    if (!wc.access_token.empty() && !wc.phone_number_id.empty())
                        cfg.channels.whatsapp = std::move(wc);
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
        } catch (...) { // NOLINT(bugprone-empty-catch)
            // Config file is malformed — continue with defaults
        }
    }

    // Environment variables always override config file
    if (const char* v = std::getenv("ANTHROPIC_API_KEY"))
        cfg.anthropic_api_key = v;
    if (const char* v = std::getenv("OPENAI_API_KEY"))
        cfg.openai_api_key = v;
    if (const char* v = std::getenv("OPENROUTER_API_KEY"))
        cfg.openrouter_api_key = v;
    if (const char* v = std::getenv("OLLAMA_BASE_URL"))
        cfg.ollama_base_url = v;
    if (const char* v = std::getenv("COMPATIBLE_BASE_URL"))
        cfg.compatible_base_url = v;

    // Channel env var overrides
    if (const char* v = std::getenv("TELEGRAM_BOT_TOKEN")) {
        if (!cfg.channels.telegram)
            cfg.channels.telegram = TelegramChannelConfig{};
        cfg.channels.telegram->bot_token = v;
    }
    if (const char* v = std::getenv("WHATSAPP_ACCESS_TOKEN")) {
        if (!cfg.channels.whatsapp)
            cfg.channels.whatsapp = WhatsAppChannelConfig{};
        cfg.channels.whatsapp->access_token = v;
    }
    if (const char* v = std::getenv("WHATSAPP_PHONE_ID")) {
        if (!cfg.channels.whatsapp)
            cfg.channels.whatsapp = WhatsAppChannelConfig{};
        cfg.channels.whatsapp->phone_number_id = v;
    }
    if (const char* v = std::getenv("WHATSAPP_VERIFY_TOKEN")) {
        if (!cfg.channels.whatsapp)
            cfg.channels.whatsapp = WhatsAppChannelConfig{};
        cfg.channels.whatsapp->verify_token = v;
    }

    return cfg;
}

std::string Config::api_key_for(const std::string& provider) const {
    if (provider == "anthropic") return anthropic_api_key;
    if (provider == "openai")    return openai_api_key;
    if (provider == "openrouter") return openrouter_api_key;
    return {};
}

std::string Config::base_url_for(const std::string& provider) const {
    if (provider == "ollama")     return ollama_base_url;
    if (provider == "compatible") return compatible_base_url;
    return {};
}

} // namespace ptrclaw
