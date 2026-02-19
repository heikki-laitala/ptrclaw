#include "provider.hpp"
#include "providers/anthropic.hpp"
#include "providers/openai.hpp"
#include "providers/ollama.hpp"
#include "providers/openrouter.hpp"
#include "providers/compatible.hpp"
#include <stdexcept>

namespace ptrclaw {

std::unique_ptr<Provider> create_provider(const std::string& name,
                                           const std::string& api_key,
                                           const std::string& base_url) {
    if (name == "anthropic") {
        return std::make_unique<AnthropicProvider>(api_key);
    } else if (name == "openai") {
        return std::make_unique<OpenAIProvider>(api_key);
    } else if (name == "ollama") {
        std::string url = base_url.empty() ? "http://localhost:11434" : base_url;
        return std::make_unique<OllamaProvider>(url);
    } else if (name == "openrouter") {
        return std::make_unique<OpenRouterProvider>(api_key);
    } else if (name == "compatible") {
        return std::make_unique<CompatibleProvider>(api_key, base_url);
    } else {
        throw std::invalid_argument("Unknown provider: " + name);
    }
}

} // namespace ptrclaw
