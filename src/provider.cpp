#include "provider.hpp"
#include "plugin.hpp"
#include "config.hpp"

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

} // namespace ptrclaw
