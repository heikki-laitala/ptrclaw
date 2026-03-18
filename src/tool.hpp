#pragma once
#include <string>
#include <memory>
#include <vector>

namespace ptrclaw {

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

protected:
    EventBus* event_bus_ = nullptr;
};

} // namespace ptrclaw
