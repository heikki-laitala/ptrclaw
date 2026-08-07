#include "compatible.hpp"
#include "../plugin.hpp"

static ptrclaw::ProviderRegistrar reg_compatible("compatible",
    [](const std::string& key, ptrclaw::HttpClient& http, const std::string& base_url,
       bool /* prompt_caching */, const ptrclaw::ProviderEntry& entry) {
        auto p = std::make_unique<ptrclaw::CompatibleProvider>(key, http, base_url);
        // Same as the openai factory: this provider speaks the OpenAI dialect, so the
        // endpoint may require a `user` — a gateway metering per caller does. Dropping it
        // here made the configured field silently absent from every request.
        p->set_user(entry.user);
        return p;
    });

namespace ptrclaw {

CompatibleProvider::CompatibleProvider(const std::string& api_key, HttpClient& http, const std::string& base_url)
    : OpenAIProvider(api_key, http, base_url) {}

} // namespace ptrclaw
