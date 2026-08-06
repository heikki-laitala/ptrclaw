#include "provider.hpp"
#include "plugin.hpp"
#include "config.hpp"
#include <cctype>
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

namespace {

// Exact ids, not name patterns: gpt-5.6-sol is served by a subscription while plain
// gpt-5.6 is API-key only, and no substring rule can express that. The lists therefore
// need editing when OpenAI ships models — providers.openai.oauth_models is the escape
// hatch that keeps a new model reachable in the meantime.
constexpr const char* kDualRouteModels[] = {
    "gpt-5.6-sol", "gpt-5.6-terra", "gpt-5.6-luna",
    "gpt-5.5", "gpt-5.5-pro",
    "gpt-5.4", "gpt-5.4-pro", "gpt-5.4-mini",
};
constexpr const char* kPlatformOnlyModels[] = {"chat-latest", "gpt-5.6"};
constexpr const char* kSubscriptionOnlyModels[] = {"gpt-5.3-codex-spark"};

std::string normalize_model_id(const std::string& model) {
    std::string id;
    id.reserve(model.size());
    for (char c : model) {
        id += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    // The only shipped alias: the 5.4 codex row was renamed.
    return id == "gpt-5.4-codex" ? "gpt-5.4" : id;
}

template <size_t N>
bool listed(const char* const (&models)[N], const std::string& id) {
    for (const char* candidate : models) {
        if (id == candidate) return true;
    }
    return false;
}

} // namespace

OpenAIModelRoute openai_model_route(const std::string& model) {
    std::string id = normalize_model_id(model);
    if (listed(kPlatformOnlyModels, id)) return OpenAIModelRoute::PlatformOnly;
    if (listed(kSubscriptionOnlyModels, id)) return OpenAIModelRoute::SubscriptionOnly;
    if (listed(kDualRouteModels, id)) return OpenAIModelRoute::Dual;
    // Codex ids that predate these lists keep the route they have always used, so a
    // release that adds a model above cannot silently move an older one off OAuth.
    if (id.find("codex") != std::string::npos) return OpenAIModelRoute::Dual;
    return OpenAIModelRoute::Unknown;
}

bool openai_oauth_eligible(const std::string& model, const ProviderEntry& entry) {
    if (!entry.oauth_models.empty()) {
        for (const auto& pattern : entry.oauth_models) {
            if (pattern == "*") return true;
            if (!pattern.empty() && model.find(pattern) != std::string::npos) return true;
        }
        return false;
    }
    auto route = openai_model_route(model);
    return route == OpenAIModelRoute::Dual || route == OpenAIModelRoute::SubscriptionOnly;
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

        if (!use_oauth) {
            // Refusing beats building a provider whose credential the model's only
            // transport does not accept: the failure would otherwise surface as an opaque
            // error from OpenAI on the next turn.
            if (openai_model_route(effective) == OpenAIModelRoute::SubscriptionOnly) {
                result.error = effective + " is served only by the ChatGPT subscription. "
                    "Run /auth openai start for OAuth.";
                return result;
            }
            if (!has_key) {
                result.error = oauth_capable
                    ? "No API key or OAuth for openai. Run /auth openai start for OAuth."
                    : (openai_model_route(effective) == OpenAIModelRoute::PlatformOnly
                       ? effective + " is served only by the OpenAI API. "
                         "Set an API key for openai."
                       : "No API key for openai");
                return result;
            }
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
