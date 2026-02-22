#pragma once
#include "openai.hpp"
#include <string>

namespace ptrclaw {

class OpenRouterProvider : public OpenAIProvider {
public:
    OpenRouterProvider(const std::string& api_key, HttpClient& http,
                       const std::string& base_url = "https://openrouter.ai/api/v1");

    std::string provider_name() const override { return "openrouter"; }

protected:
    std::vector<Header> build_headers() const override;
};

} // namespace ptrclaw
