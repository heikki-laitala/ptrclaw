#pragma once
#include "../memory.hpp"

namespace ptrclaw {

class MemoryStoreTool : public MemoryAwareTool {
public:
    ToolResult execute(const std::string& args_json) override;
    std::string tool_name() const override { return "memory_store"; }
    std::string description() const override;
    std::string parameters_json() const override;
};

} // namespace ptrclaw
