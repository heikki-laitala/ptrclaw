#pragma once
#include "memory.hpp"
#include "memory/response_cache.hpp"
#include "provider.hpp"
#include "tool.hpp"
#include "config.hpp"
#include <string>
#include <vector>
#include <memory>

namespace ptrclaw {

class EventBus; // forward declaration

class Agent {
public:
    Agent(std::unique_ptr<Provider> provider,
          std::vector<std::unique_ptr<Tool>> tools,
          const Config& config);

    // Process a user message and return the assistant's final text reply
    std::string process(const std::string& user_message);

    // Get current history size
    size_t history_size() const { return history_.size(); }

    // Get estimated token usage
    uint32_t estimated_tokens() const;

    // Clear history
    void clear_history();

    // Switch model
    void set_model(const std::string& model) { model_ = model; }
    const std::string& model() const { return model_; }

    // Switch provider
    void set_provider(std::unique_ptr<Provider> provider);
    std::string provider_name() const;

    // Optional event bus integration (nullptr = disabled)
    void set_event_bus(EventBus* bus) { event_bus_ = bus; }
    void set_session_id(const std::string& id) { session_id_ = id; }
    void set_channel(const std::string& ch) { channel_ = ch; }
    void set_binary_path(const std::string& path) { binary_path_ = path; }

    // Memory system
    void set_memory(std::unique_ptr<Memory> memory);
    Memory* memory() const { return memory_.get(); }

    // Response cache
    void set_response_cache(std::unique_ptr<ResponseCache> cache);

    // Soul hatching
    bool is_hatched() const;
    void start_hatch();
    bool hatching() const { return hatching_; }

private:
    void compact_history();
    void inject_system_prompt();
    void wire_memory_tools();
    void maybe_synthesize();

    std::unique_ptr<Provider> provider_;
    std::vector<std::unique_ptr<Tool>> tools_;
    std::vector<ChatMessage> history_;
    Config config_;
    std::string model_;
    bool system_prompt_injected_ = false;
    EventBus* event_bus_ = nullptr;
    std::string session_id_;
    std::string channel_;
    std::string binary_path_;
    std::unique_ptr<Memory> memory_;
    std::unique_ptr<ResponseCache> response_cache_;
    uint32_t turns_since_synthesis_ = 0;
    bool hatching_ = false;
    uint32_t last_prompt_tokens_ = 0; // from provider usage when available
    bool has_last_prompt_tokens_ = false;
};

} // namespace ptrclaw
