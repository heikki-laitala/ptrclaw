#include "provider.hpp"
#include "plugin.hpp"

namespace ptrclaw {

std::unique_ptr<Provider> create_provider(const std::string& name,
                                           const std::string& api_key,
                                           HttpClient& http,
                                           const std::string& base_url,
                                           bool prompt_caching) {
    return PluginRegistry::instance().create_provider(
        name, api_key, http, base_url, prompt_caching);
}

} // namespace ptrclaw
