#include "provider.hpp"
#include "plugin.hpp"
#include "config.hpp"
#ifdef PTRCLAW_HAS_OPENAI
#include "providers/openai_token_persist.hpp"
#endif

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

bool openai_oauth_eligible(const std::string& model, const ProviderEntry& entry) {
    if (!entry.oauth_models.empty()) {
        for (const auto& pattern : entry.oauth_models) {
            if (pattern == "*") return true;
            if (!pattern.empty() && model.find(pattern) != std::string::npos) return true;
        }
        return false;
    }
    return model.find("codex") != std::string::npos ||
           model.find("gpt-5") != std::string::npos;
}

std::string auth_mode_label(const std::string& provider_name,
                             const std::string& model,
                             const Config& config) {
    if (provider_name == "openai") {
        auto it = config.providers.find("openai");
        if (it != config.providers.end() && !it->second.oauth_access_token.empty() &&
            openai_oauth_eligible(model, it->second))
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

    // OpenAI: models the subscription can serve prefer OAuth when tokens are present and
    // fall back to the API key. Everything else must use the API key — the ChatGPT
    // backend will not serve it, and api.openai.com does not accept these tokens.
    if (name == "openai") {
        std::string effective = model_arg.empty() ? current_model : model_arg;
        bool oauth_capable = openai_oauth_eligible(effective, entry);
        bool has_oauth = !entry.oauth_access_token.empty();
        bool has_key = !entry.api_key.empty();
        bool use_oauth = oauth_capable && has_oauth;

        if (!use_oauth && !has_key) {
            result.error = oauth_capable
                ? "No API key or OAuth for openai. Run /auth openai start for OAuth."
                : "No API key for openai";
            return result;
        }

        ProviderEntry adjusted = entry;
        adjusted.use_oauth = use_oauth;
        // Record the mode actually built: callers compare against this to decide whether a
        // model change needs a new provider, and a stale flag skips that rebuild.
        it->second.use_oauth = use_oauth;
        result.provider = create_provider("openai", config.api_key_for("openai"), http,
            config.base_url_for("openai"), config.prompt_caching_for("openai"), &adjusted);
        result.model = model_arg.empty() ? effective : model_arg;
#ifdef PTRCLAW_HAS_OPENAI
        // Guarded on the provider, not the interactive flow: a build without the
        // flow can still be given OAuth tokens in config, and OpenAIProvider still
        // refreshes them. Skipping this would drop the rotated refresh token, so
        // the next restart would load a stale one and fail to authenticate.
        setup_oauth_refresh(result.provider.get(), config);
#endif
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
