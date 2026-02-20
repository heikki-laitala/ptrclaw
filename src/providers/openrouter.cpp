#include "openrouter.hpp"
#include "../plugin.hpp"

static ptrclaw::ProviderRegistrar reg_openrouter("openrouter",
    [](const std::string& key, ptrclaw::HttpClient& http, const std::string&) {
        return std::make_unique<ptrclaw::OpenRouterProvider>(key, http);
    });

namespace ptrclaw {

OpenRouterProvider::OpenRouterProvider(const std::string& api_key, HttpClient& http)
    : OpenAIProvider(api_key, http, "https://openrouter.ai/api/v1") {}

std::vector<Header> OpenRouterProvider::build_headers() const {
    auto headers = OpenAIProvider::build_headers();
    headers.emplace_back("HTTP-Referer", "https://ptrclaw.dev");
    headers.emplace_back("X-Title", "PtrClaw");
    return headers;
}

} // namespace ptrclaw
