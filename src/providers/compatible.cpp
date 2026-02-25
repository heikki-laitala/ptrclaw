#include "compatible.hpp"
#include "../plugin.hpp"

static ptrclaw::ProviderRegistrar reg_compatible("compatible",
    [](const std::string& key, ptrclaw::HttpClient& http, const std::string& base_url,
       bool /* prompt_caching */) {
        return std::make_unique<ptrclaw::CompatibleProvider>(key, http, base_url);
    });

namespace ptrclaw {

CompatibleProvider::CompatibleProvider(const std::string& api_key, HttpClient& http, const std::string& base_url)
    : OpenAIProvider(api_key, http, base_url) {}

} // namespace ptrclaw
