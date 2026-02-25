#include "openrouter.hpp"
#include "../plugin.hpp"

static ptrclaw::ProviderRegistrar reg_openrouter("openrouter",
    [](const std::string& key, ptrclaw::HttpClient& http, const std::string& base_url,
       bool /* prompt_caching */) {
        return std::make_unique<ptrclaw::OpenRouterProvider>(key, http, base_url);
    });

namespace ptrclaw {

OpenRouterProvider::OpenRouterProvider(const std::string& api_key, HttpClient& http,
                                       const std::string& base_url)
    : OpenAIProvider(api_key, http,
                     base_url.empty() ? "https://openrouter.ai/api/v1" : base_url) {}

std::vector<Header> OpenRouterProvider::build_headers() const {
    auto headers = OpenAIProvider::build_headers();
    headers.emplace_back("HTTP-Referer", "https://ptrclaw.dev");
    headers.emplace_back("X-Title", "PtrClaw");
    return headers;
}

} // namespace ptrclaw
