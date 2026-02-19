#include "reliable.hpp"
#include <iostream>
#include <stdexcept>

namespace ptrclaw {

ReliableProvider::ReliableProvider(std::vector<std::unique_ptr<Provider>> providers,
                                   uint32_t max_retries)
    : providers_(std::move(providers)), max_retries_(max_retries) {
    if (providers_.empty()) {
        throw std::invalid_argument("ReliableProvider requires at least one provider");
    }
}

ChatResponse ReliableProvider::chat(const std::vector<ChatMessage>& messages,
                                     const std::vector<ToolSpec>& tools,
                                     const std::string& model,
                                     double temperature) {
    std::string last_error;
    for (size_t i = 0; i < providers_.size(); ++i) {
        for (uint32_t retry = 0; retry < max_retries_; ++retry) {
            try {
                return providers_[i]->chat(messages, tools, model, temperature);
            } catch (const std::exception& e) {
                last_error = e.what();
                std::cerr << "[reliable] Provider " << providers_[i]->provider_name()
                          << " attempt " << (retry + 1) << "/" << max_retries_
                          << " failed: " << last_error << '\n';
            }
        }
    }
    throw std::runtime_error("All providers failed. Last error: " + last_error);
}

std::string ReliableProvider::chat_simple(const std::string& system_prompt,
                                           const std::string& message,
                                           const std::string& model,
                                           double temperature) {
    std::string last_error;
    for (size_t i = 0; i < providers_.size(); ++i) {
        for (uint32_t retry = 0; retry < max_retries_; ++retry) {
            try {
                return providers_[i]->chat_simple(system_prompt, message, model, temperature);
            } catch (const std::exception& e) {
                last_error = e.what();
                std::cerr << "[reliable] Provider " << providers_[i]->provider_name()
                          << " attempt " << (retry + 1) << "/" << max_retries_
                          << " failed: " << last_error << '\n';
            }
        }
    }
    throw std::runtime_error("All providers failed. Last error: " + last_error);
}

bool ReliableProvider::supports_native_tools() const {
    return providers_[0]->supports_native_tools();
}

bool ReliableProvider::supports_streaming() const {
    return providers_[0]->supports_streaming();
}

} // namespace ptrclaw
