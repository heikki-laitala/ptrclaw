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

    bool on_codex = false;
    if (current_provider == "openai") {
        auto it = config.providers.find("openai");
        on_codex = (it != config.providers.end() && it->second.use_oauth);
    }
    std::vector<ProviderInfo> result;

    for (const auto& [name, entry] : config.providers) {
        if (name == "openai") {
            if (!entry.api_key.empty()) {
                result.push_back({
                    "openai", "API key",
                    current_provider == "openai" && !on_codex});
            }
            if (!entry.oauth_access_token.empty()) {
                result.push_back({"openai-codex", "OAuth", on_codex});
            }
            continue;
        }
        if (!entry.api_key.empty()) {
            result.push_back({name, "API key", name == current_provider});
        } else if (!entry.base_url.empty() && name == current_provider) {
            // base_url-only providers (ollama, compatible) — only show when active
            result.push_back({name, "local", true});
        }
    }
    return result;
}

SwitchProviderResult switch_provider(const std::string& name,
                                     const std::string& model_arg,
                                     Config& config,
                                     HttpClient& http) {
    SwitchProviderResult result;
    result.display_name = name;

    if (name == "openai-codex") {
        auto it = config.providers.find("openai");
        if (it == config.providers.end() || it->second.oauth_access_token.empty()) {
            result.error = "OpenAI OAuth not configured. Run /auth openai start";
            return result;
        }
        result.provider = create_provider("openai", config.api_key_for("openai"), http,
            config.base_url_for("openai"), config.prompt_caching_for("openai"), &it->second);
        result.model = model_arg.empty() ? std::string(kDefaultOAuthModel) : model_arg;
        return result;
    }

    auto it = config.providers.find(name);
    if (it == config.providers.end()) {
        result.error = "Unknown provider: " + name;
        return result;
    }
    if (it->second.api_key.empty() && it->second.base_url.empty()) {
        result.error = "No credentials for " + name;
        return result;
    }

    const ProviderEntry* ep = &it->second;
    ProviderEntry no_oauth;
    if (name == "openai") {
        no_oauth = it->second;
        no_oauth.use_oauth = false;
        ep = &no_oauth;
    }
    result.provider = create_provider(name, config.api_key_for(name), http,
        config.base_url_for(name), config.prompt_caching_for(name), ep);
    result.model = model_arg;
    return result;
}

} // namespace ptrclaw
