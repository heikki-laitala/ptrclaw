#include "compatible.hpp"

namespace ptrclaw {

CompatibleProvider::CompatibleProvider(const std::string& api_key, HttpClient& http, const std::string& base_url)
    : OpenAIProvider(api_key, http, base_url) {}

} // namespace ptrclaw
