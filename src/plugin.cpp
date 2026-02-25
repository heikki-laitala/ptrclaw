#include "plugin.hpp"
#include "memory.hpp"
#include <stdexcept>
#include <algorithm>

namespace ptrclaw {

PluginRegistry& PluginRegistry::instance() {
    static PluginRegistry registry;
    return registry;
}

void PluginRegistry::register_provider(const std::string& name, ProviderFactory factory) {
    std::lock_guard<std::mutex> lock(mutex_);
    providers_[name] = std::move(factory);
}

void PluginRegistry::register_tool(const std::string& name, ToolFactory factory) {
    std::lock_guard<std::mutex> lock(mutex_);
    tools_[name] = std::move(factory);
}

void PluginRegistry::register_channel(const std::string& name, ChannelFactory factory) {
    std::lock_guard<std::mutex> lock(mutex_);
    channels_[name] = std::move(factory);
}

void PluginRegistry::register_memory(const std::string& name, MemoryFactory factory) {
    std::lock_guard<std::mutex> lock(mutex_);
    memories_[name] = std::move(factory);
}

std::unique_ptr<Provider> PluginRegistry::create_provider(const std::string& name,
                                                           const std::string& api_key,
                                                           HttpClient& http,
                                                           const std::string& base_url,
                                                           bool prompt_caching) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = providers_.find(name);
    if (it == providers_.end()) {
        throw std::invalid_argument("Unknown provider: " + name);
    }
    return it->second(api_key, http, base_url, prompt_caching);
}

std::vector<std::unique_ptr<Tool>> PluginRegistry::create_all_tools() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::unique_ptr<Tool>> result;
    result.reserve(tools_.size());
    for (const auto& [name, factory] : tools_) {
        result.push_back(factory());
    }
    return result;
}

std::unique_ptr<Channel> PluginRegistry::create_channel(const std::string& name,
                                                         const Config& config,
                                                         HttpClient& http) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = channels_.find(name);
    if (it == channels_.end()) {
        throw std::invalid_argument("Unknown channel: " + name);
    }
    return it->second(config, http);
}

std::vector<std::string> PluginRegistry::provider_names() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> names;
    names.reserve(providers_.size());
    for (const auto& [name, _] : providers_) {
        names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::vector<std::string> PluginRegistry::tool_names() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> names;
    names.reserve(tools_.size());
    for (const auto& [name, _] : tools_) {
        names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::vector<std::string> PluginRegistry::channel_names() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> names;
    names.reserve(channels_.size());
    for (const auto& [name, _] : channels_) {
        names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

bool PluginRegistry::has_provider(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return providers_.count(name) > 0;
}

bool PluginRegistry::has_channel(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return channels_.count(name) > 0;
}

std::unique_ptr<Memory> PluginRegistry::create_memory(const std::string& name,
                                                       const Config& config) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = memories_.find(name);
    if (it == memories_.end()) {
        throw std::invalid_argument("Unknown memory backend: " + name);
    }
    return it->second(config);
}

bool PluginRegistry::has_memory(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return memories_.count(name) > 0;
}

void PluginRegistry::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    providers_.clear();
    tools_.clear();
    channels_.clear();
    memories_.clear();
}

} // namespace ptrclaw
