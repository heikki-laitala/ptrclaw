#include "provider.hpp"
#include "plugin.hpp"
#include "config.hpp"
#include "oauth.hpp"

namespace ptrclaw {

std::unique_ptr<Provider> create_provider(const std::string& name,
                                           const std::string& api_key,
                                           HttpClient& http,
                                           const std::string& base_url,
                                           bool prompt_caching,
                                           const ProviderEntry* provider_entry) {
    static const ProviderEntry kDefaultProviderEntry{};
    const ProviderEntry& entry = provider_entry ? *provider_entry : kDefaultProviderEntry;
    return PluginRegistry::instance().create_provider(
        name, api_key, http, base_url, prompt_caching, entry);
}

std::vector<ProviderInfo> list_providers(
    const Config& config,
    const std::string& current_provider) {

    std::vector<ProviderInfo> result;

    for (const auto& [name, entry] : config.providers) {
        if (name == "openai") {
            bool has_key = !entry.api_key.empty();
            bool has_oauth = !entry.oauth_access_token.empty();
            if (!has_key && !has_oauth) continue;
            result.push_back({name, name == current_provider,
                              has_key, has_oauth, false});
            continue;
        }
        if (!entry.api_key.empty()) {
            result.push_back({name, name == current_provider,
                              true, false, false});
        } else if (!entry.base_url.empty() && name == current_provider) {
            result.push_back({name, true, false, false, true});
        }
    }
    return result;
}

std::string auth_mode_label(const std::string& provider_name,
                             const std::string& model,
                             const Config& config) {
    if (provider_name == "openai") {
        if (model.find("codex") != std::string::npos)
            return "OAuth";
        return "API key";
    }
    auto it = config.providers.find(provider_name);
    if (it != config.providers.end() && it->second.api_key.empty())
        return "local";
    return "API key";
}

SwitchProviderResult switch_provider(const std::string& name,
                                     const std::string& model_arg,
                                     const std::string& current_model,
                                     Config& config,
                                     HttpClient& http) {
    SwitchProviderResult result;

    auto it = config.providers.find(name);
    if (it == config.providers.end()) {
        result.error = "Unknown provider: " + name;
        return result;
    }

    const auto& entry = it->second;

    // OpenAI: auto-select OAuth vs API key based on model name
    if (name == "openai") {
        std::string effective = model_arg.empty() ? current_model : model_arg;
        bool needs_oauth = effective.find("codex") != std::string::npos;

        if (needs_oauth) {
            if (entry.oauth_access_token.empty()) {
                result.error = "OpenAI OAuth not configured. Run /auth openai start";
                return result;
            }
            result.provider = create_provider("openai", config.api_key_for("openai"), http,
                config.base_url_for("openai"), config.prompt_caching_for("openai"), &entry);
            result.model = model_arg.empty() ? effective : model_arg;
        } else {
            if (entry.api_key.empty()) {
                result.error = "No API key for openai";
                return result;
            }
            ProviderEntry no_oauth = entry;
            no_oauth.use_oauth = false;
            result.provider = create_provider("openai", config.api_key_for("openai"), http,
                config.base_url_for("openai"), config.prompt_caching_for("openai"), &no_oauth);
            result.model = model_arg;
        }
        return result;
    }

    if (entry.api_key.empty() && entry.base_url.empty()) {
        result.error = "No credentials for " + name;
        return result;
    }

    result.provider = create_provider(name, config.api_key_for(name), http,
        config.base_url_for(name), config.prompt_caching_for(name), &entry);
    result.model = model_arg;
    return result;
}

} // namespace ptrclaw
