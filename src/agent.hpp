#pragma once
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

private:
    void compact_history();
    void inject_system_prompt();

    std::unique_ptr<Provider> provider_;
    std::vector<std::unique_ptr<Tool>> tools_;
    std::vector<ChatMessage> history_;
    Config config_;
    std::string model_;
    bool system_prompt_injected_ = false;
    EventBus* event_bus_ = nullptr;
    std::string session_id_;
};

} // namespace ptrclaw
