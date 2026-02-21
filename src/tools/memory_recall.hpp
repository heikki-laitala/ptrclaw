#pragma once
#include "../memory.hpp"

namespace ptrclaw {

class MemoryRecallTool : public MemoryAwareTool {
public:
    ToolResult execute(const std::string& args_json) override;
    std::string tool_name() const override { return "memory_recall"; }
    std::string description() const override;
    std::string parameters_json() const override;
};

} // namespace ptrclaw
