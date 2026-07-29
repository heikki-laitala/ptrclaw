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
    oai->set_on_token_refresh(
        [&config](const std::string& at, const std::string& rt, uint64_t ea) {
            auto& entry = config.providers["openai"];
            entry.oauth_access_token = at;
            entry.oauth_refresh_token = rt;
            entry.oauth_expires_at = ea;
            persist_openai_oauth(entry);
        });
}

} // namespace ptrclaw
