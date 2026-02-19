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
        } catch (...) {
            // If parsing fails, continue with defaults
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

    return cfg;
}

std::string Config::api_key_for(const std::string& provider) const {
    if (provider == "anthropic") return anthropic_api_key;
    if (provider == "openai")    return openai_api_key;
    if (provider == "openrouter") return openrouter_api_key;
    return {};
}

} // namespace ptrclaw
