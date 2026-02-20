#pragma once
#include "provider.hpp"
#include "tool.hpp"
#include "channel.hpp"
#include "http.hpp"
#include "config.hpp"
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <unordered_map>
#include <mutex>

namespace ptrclaw {

// Factory function types
using ProviderFactory = std::function<std::unique_ptr<Provider>(
    const std::string& api_key, HttpClient& http, const std::string& base_url)>;

using ToolFactory = std::function<std::unique_ptr<Tool>()>;

using ChannelFactory = std::function<std::unique_ptr<Channel>(
    const Config& config, HttpClient& http)>;

// Central registry for self-registering plugins.
// All methods are thread-safe.
class PluginRegistry {
public:
    static PluginRegistry& instance();

    // Registration
    void register_provider(const std::string& name, ProviderFactory factory);
    void register_tool(const std::string& name, ToolFactory factory);
    void register_channel(const std::string& name, ChannelFactory factory);

    // Creation
    std::unique_ptr<Provider> create_provider(const std::string& name,
                                              const std::string& api_key,
                                              HttpClient& http,
                                              const std::string& base_url) const;

    std::vector<std::unique_ptr<Tool>> create_all_tools() const;

    std::unique_ptr<Channel> create_channel(const std::string& name,
                                            const Config& config,
                                            HttpClient& http) const;

    // Query
    std::vector<std::string> provider_names() const;
    std::vector<std::string> tool_names() const;
    std::vector<std::string> channel_names() const;
    bool has_provider(const std::string& name) const;
    bool has_channel(const std::string& name) const;

    // Testing support
    void clear();

private:
    PluginRegistry() = default;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, ProviderFactory> providers_;
    std::unordered_map<std::string, ToolFactory> tools_;
    std::unordered_map<std::string, ChannelFactory> channels_;
};

// ── Self-registrar helpers (used at file scope in each plugin .cpp) ──

struct ProviderRegistrar {
    ProviderRegistrar(const std::string& name, ProviderFactory factory) {
        PluginRegistry::instance().register_provider(name, std::move(factory));
    }
};

struct ToolRegistrar {
    ToolRegistrar(const std::string& name, ToolFactory factory) {
        PluginRegistry::instance().register_tool(name, std::move(factory));
    }
};

struct ChannelRegistrar {
    ChannelRegistrar(const std::string& name, ChannelFactory factory) {
        PluginRegistry::instance().register_channel(name, std::move(factory));
    }
};

} // namespace ptrclaw
