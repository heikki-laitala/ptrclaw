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
        if (model.find("codex") != std::string::npos) {
            auto it = config.providers.find("openai");
            if (it != config.providers.end() && !it->second.oauth_access_token.empty())
                return "OAuth";
        }
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

    // OpenAI: codex models prefer OAuth when available, fall back to API key.
    // Non-codex models always use API key.
    if (name == "openai") {
        std::string effective = model_arg.empty() ? current_model : model_arg;
        bool is_codex = effective.find("codex") != std::string::npos;
        bool has_oauth = !entry.oauth_access_token.empty();
        bool has_key = !entry.api_key.empty();
        bool use_oauth = is_codex && has_oauth;

        if (!use_oauth && !has_key) {
            result.error = is_codex
                ? "No API key or OAuth for openai. Run /auth openai start for OAuth."
                : "No API key for openai";
            return result;
        }

        ProviderEntry adjusted = entry;
        adjusted.use_oauth = use_oauth;
        result.provider = create_provider("openai", config.api_key_for("openai"), http,
            config.base_url_for("openai"), config.prompt_caching_for("openai"), &adjusted);
        result.model = model_arg.empty() ? effective : model_arg;
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
