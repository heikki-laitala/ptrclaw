#pragma once
#include "openai.hpp"
#include <string>

namespace ptrclaw {

// OpenAI-compatible provider: same as OpenAI but with a custom base URL
class CompatibleProvider : public OpenAIProvider {
public:
    CompatibleProvider(const std::string& api_key, const std::string& base_url);

    std::string provider_name() const override { return "compatible"; }
};

} // namespace ptrclaw
