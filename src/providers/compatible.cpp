#include "compatible.hpp"

namespace ptrclaw {

CompatibleProvider::CompatibleProvider(const std::string& api_key, const std::string& base_url)
    : OpenAIProvider(api_key, base_url) {}

} // namespace ptrclaw
