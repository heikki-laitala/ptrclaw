#include "commands.hpp"
#include "agent.hpp"
#include "config.hpp"
#include "http.hpp"
#include "memory.hpp"
#include "onboard.hpp"
#include "prompt.hpp"
#include "provider.hpp"
#include "util.hpp"

#include <fstream>

namespace ptrclaw {

std::string cmd_status(const Agent& agent) {
    return "Provider: " + agent.provider_name() + "\n"
        + "Model: " + agent.model() + "\n"
        + "History: " + std::to_string(agent.history_size()) + " messages\n"
        + "Estimated tokens: " + std::to_string(agent.estimated_tokens()) + "\n";
}

std::string cmd_models(const Agent& agent, const Config& config) {
    std::string auth_mode = auth_mode_label(
        agent.provider_name(), agent.model(), config);
    std::string result = "Current: " + agent.provider_name()
        + " \xe2\x80\x94 " + agent.model()
        + " (" + auth_mode + ")\n\nProviders:\n";

    auto infos = list_providers(config, agent.provider_name());
    for (const auto& info : infos) {
        result += "  " + info.name + " \xe2\x80\x94 ";
        if (info.has_api_key) result += "API key";
        if (info.has_api_key && info.has_oauth) result += ", ";
        if (info.has_oauth) result += "OAuth (codex models)";
        if (info.is_local) result += "local";
        result += "\n";
    }
    result += "\nSwitch: /provider <name> [model]";
    return result;
}

std::string cmd_memory(const Agent& agent) {
    auto* mem = agent.memory();
    if (!mem || mem->backend_name() == "none") {
        return "Memory: disabled";
    }
    return "Memory backend: " + std::string(mem->backend_name()) + "\n"
        + "  Core:         " + std::to_string(mem->count(MemoryCategory::Core)) + " entries\n"
        + "  Knowledge:    " + std::to_string(mem->count(MemoryCategory::Knowledge)) + " entries\n"
        + "  Conversation: " + std::to_string(mem->count(MemoryCategory::Conversation)) + " entries\n"
        + "  Total:        " + std::to_string(mem->count(std::nullopt)) + " entries\n";
}

std::string cmd_soul(const Agent& agent, bool dev) {
    if (!dev) return "Unknown command: /soul";
    std::string display;
    if (agent.memory()) {
        display = format_soul_display(agent.memory());
    }
    return display.empty() ? "No soul data yet. Use /hatch to create one." : display;
}

std::string cmd_hatch(Agent& agent) {
    agent.start_hatch();
    return agent.process("Begin the hatching interview.");
}

std::string cmd_model(const std::string& new_model, Agent& agent,
                       Config& config, HttpClient& http) {
    // On openai, re-create provider if auth mode changes
#ifdef PTRCLAW_HAS_OPENAI
    if (agent.provider_name() == "openai") {
        auto oai_it = config.providers.find("openai");
        bool on_oauth = oai_it != config.providers.end() &&
                        oai_it->second.use_oauth;
        bool want_oauth = new_model.find("codex") != std::string::npos &&
                          oai_it != config.providers.end() &&
                          !oai_it->second.oauth_access_token.empty();
        if (on_oauth != want_oauth) {
            auto sr = switch_provider("openai", new_model, agent.model(),
                                       config, http);
            if (!sr.error.empty()) return sr.error;
            agent.set_provider(std::move(sr.provider));
            if (!sr.model.empty()) agent.set_model(sr.model);
            config.model = agent.model();
            config.persist_selection();
            return "Model set to: " + agent.model();
        }
    }
#endif
    agent.set_model(new_model);
    config.model = new_model;
    config.persist_selection();
    return "Model set to: " + new_model;
}

std::string cmd_provider(const std::string& args_str, Agent& agent,
                          Config& config, HttpClient& http) {
    auto args = trim(args_str);
    auto space = args.find(' ');
    std::string prov_name = (space == std::string::npos)
        ? args : args.substr(0, space);
    std::string model_arg = (space == std::string::npos)
        ? "" : trim(args.substr(space + 1));

    auto sr = switch_provider(prov_name, model_arg, agent.model(), config, http);
    if (!sr.error.empty()) return sr.error;

    agent.set_provider(std::move(sr.provider));
    if (!sr.model.empty()) agent.set_model(sr.model);
    config.provider = prov_name;
    config.model = agent.model();
    config.persist_selection();
    return "Switched to " + prov_name + " | Model: " + agent.model();
}

std::string cmd_skill(const std::string& args, Agent& agent) {
    agent.load_skills(); // re-scan directory for new/changed skills
    auto trimmed = trim(args);
    const auto& skills = agent.available_skills();

    // /skill (no args) — list available skills
    if (trimmed.empty()) {
        if (skills.empty()) {
            return "No skills available. Add .md files to ~/.ptrclaw/skills/";
        }
        const std::string& active_name = agent.active_skill_name();
        std::string result = "Skills:\n";
        for (const auto& s : skills) {
            bool active = (s.name == active_name);
            result += active ? "* " : "  ";
            result += s.name;
            if (!s.description.empty()) result += " \xe2\x80\x94 " + s.description;
            if (active) {
                result += " [active]";
            } else {
                result += " [off]";
            }
            result += "\n";
        }
        result += "\nActivate: /skill <name>  |  Deactivate: /skill off";
        return result;
    }

    // /skill off [name] — deactivate
    if (trimmed == "off" || trimmed == "none" ||
        trimmed.rfind("off ", 0) == 0 || trimmed.rfind("none ", 0) == 0) {
        agent.deactivate_skill();
        return "Skill deactivated";
    }

    // /skill <name> — activate
    if (agent.activate_skill(trimmed)) {
        return "Skill '" + trimmed + "' activated";
    }
    return "Unknown skill: " + trimmed;
}

std::string cmd_help(bool dev, bool channel) {
    std::string result =
        "Commands:\n"
        "  /new             Clear conversation history\n"
        "  /status          Show current status\n"
        "  /model X         Switch to model X\n"
        "  /models          List configured providers\n"
        "  /provider X [M]  Switch to provider X, optional model M\n"
        "  /skill [name]    List or activate skills\n"
        "  /memory          Show memory status\n";
    if (!channel) {
        result +=
            "  /memory export   Export memories as JSON\n"
            "  /memory import P Import memories from JSON file\n";
    }
    result +=
        "  /auth            Show auth status for all providers\n"
        "  /auth <prov> <key>  Set API key\n"
        "  /auth openai start  Begin OAuth flow\n";
    if (dev) {
        result += "  /soul            Show current soul/identity data\n";
    }
    result += "  /hatch           Create or re-create assistant identity\n";
    if (!channel) {
        result +=
            "  /onboard         Run setup wizard\n"
            "  /exit, /quit     Exit\n";
    }
    result += "  /help            Show this help\n";
    return result;
}

std::string cmd_memory_export(const Agent& agent) {
    auto* mem = agent.memory();
    if (!mem || mem->backend_name() == "none") {
        return "Memory: disabled";
    }
    return mem->snapshot_export();
}

std::string cmd_memory_import(Agent& agent, const std::string& path) {
    auto* mem = agent.memory();
    if (!mem || mem->backend_name() == "none") {
        return "Memory: disabled";
    }
    std::ifstream file(path);
    if (!file.is_open()) {
        return "Error: cannot open " + path;
    }
    std::string content((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
    uint32_t n = mem->snapshot_import(content);
    return "Imported " + std::to_string(n) + " entries.";
}

std::string cmd_auth_status(const Config& config) {
    return format_auth_status(config) + "\nSet credentials: /auth <provider>";
}

} // namespace ptrclaw
