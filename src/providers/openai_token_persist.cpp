#include "providers/openai_token_persist.hpp"
#include "provider.hpp"
#include "providers/openai.hpp"

#include <nlohmann/json.hpp>

namespace ptrclaw {

using json = nlohmann::json;

bool persist_openai_oauth(const ProviderEntry& entry) {
    return modify_config_json([&](json& j) {
        if (!j.contains("providers") || !j["providers"].is_object())
            j["providers"] = json::object();
        if (!j["providers"].contains("openai") || !j["providers"]["openai"].is_object())
            j["providers"]["openai"] = json::object();

        auto& o = j["providers"]["openai"];
        o["use_oauth"] = entry.use_oauth;
        o["oauth_access_token"] = entry.oauth_access_token;
        o["oauth_refresh_token"] = entry.oauth_refresh_token;
        o["oauth_expires_at"] = entry.oauth_expires_at;
        o["oauth_client_id"] = entry.oauth_client_id;
        o["oauth_token_url"] = entry.oauth_token_url;
    });
}

void setup_oauth_refresh(Provider* provider, Config& config) {
    auto* oai = dynamic_cast<OpenAIProvider*>(provider);
    if (!oai) return;
    // Lets a provider that waited on oauth_refresh_mutex() adopt whatever another
    // session's provider just rotated, instead of spending its own stale refresh
    // token. Called from inside a refresh, so it takes only the credentials lock.
    oai->set_on_token_reload(
        [&config](std::string& at, std::string& rt, uint64_t& ea) {
            std::lock_guard<std::mutex> lock(provider_credentials_mutex());
            auto it = config.providers.find("openai");
            if (it == config.providers.end()) return false;
            at = it->second.oauth_access_token;
            rt = it->second.oauth_refresh_token;
            ea = it->second.oauth_expires_at;
            return true;
        });

    oai->set_on_token_refresh(
        [&config](const std::string& at, const std::string& rt, uint64_t ea) {
            // Every session's provider installs this callback over the same
            // Config, and with workers > 1 it fires on whichever worker hit the
            // expiry. Two turns expiring at once would race on these strings and
            // on the config file's shared temp path — see
            // provider_credentials_mutex(). Held across the write so memory and
            // file cannot disagree.
            std::lock_guard<std::mutex> lock(provider_credentials_mutex());
            auto& entry = config.providers["openai"];
            entry.oauth_access_token = at;
            entry.oauth_refresh_token = rt;
            entry.oauth_expires_at = ea;
            persist_openai_oauth(entry);
        });
}

} // namespace ptrclaw
