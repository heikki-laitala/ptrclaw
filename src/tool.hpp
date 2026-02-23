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

// Create all built-in tools
std::vector<std::unique_ptr<Tool>> create_builtin_tools();

} // namespace ptrclaw
