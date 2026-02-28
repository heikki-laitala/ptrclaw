#include "commands.hpp"
#include "agent.hpp"
#include "config.hpp"
#include "http.hpp"
#include "memory.hpp"
#include "oauth.hpp"
#include "prompt.hpp"
#include "provider.hpp"
#include "util.hpp"

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
            setup_oauth_refresh(sr.provider.get(), config);
            agent.set_provider(std::move(sr.provider));
            if (!sr.model.empty()) agent.set_model(sr.model);
            config.model = agent.model();
            config.persist_selection();
            return "Model set to: " + agent.model();
        }
    }
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

    setup_oauth_refresh(sr.provider.get(), config);
    agent.set_provider(std::move(sr.provider));
    if (!sr.model.empty()) agent.set_model(sr.model);
    config.provider = prov_name;
    config.model = agent.model();
    config.persist_selection();
    return "Switched to " + prov_name + " | Model: " + agent.model();
}

} // namespace ptrclaw
