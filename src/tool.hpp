#pragma once
#include <string>
#include <memory>
#include <vector>
#include <atomic>

namespace ptrclaw {

// Cooperative cancellation token. Tools check this periodically during long
// operations and bail out early when signalled.
using CancellationToken = std::shared_ptr<std::atomic<bool>>;

inline CancellationToken make_cancellation_token() {
    return std::make_shared<std::atomic<bool>>(false);
}

inline bool is_cancelled(const CancellationToken& token) {
    return token && token->load(std::memory_order_relaxed);
}

inline void cancel(const CancellationToken& token) {
    if (token) token->store(true, std::memory_order_relaxed);
}

struct ToolSpec {
    std::string name;
    std::string description;
    std::string parameters_json; // JSON schema for parameters
};

struct ToolResult {
    bool success;
    std::string output;
};

class Tool {
public:
    virtual ~Tool() = default;
    virtual ToolResult execute(const std::string& args_json) = 0;
    virtual ToolResult execute(const std::string& args_json,
                               const CancellationToken& /*token*/) {
        return execute(args_json);
    }
    virtual std::string tool_name() const = 0;
    virtual std::string description() const = 0;
    virtual std::string parameters_json() const = 0;
    virtual void reset() {}

    ToolSpec spec() const {
        return ToolSpec{tool_name(), description(), parameters_json()};
    }
};

// Check if a tool name is a memory tool (memory_store, memory_recall, etc.)
inline bool is_memory_tool(const std::string& name) {
    return name == "memory_store" || name == "memory_recall" ||
           name == "memory_forget" || name == "memory_link";
}

// Create all built-in tools
std::vector<std::unique_ptr<Tool>> create_builtin_tools();

class EventBus; // forward declaration

// Base class for tools that need EventBus access for request/response patterns.
class EventBusAwareTool : public Tool {
public:
    void set_event_bus(EventBus* bus) { event_bus_ = bus; }
    void set_session_id(const std::string& id) { session_id_ = id; }

protected:
    EventBus* event_bus_ = nullptr;
    std::string session_id_;
};

} // namespace ptrclaw
