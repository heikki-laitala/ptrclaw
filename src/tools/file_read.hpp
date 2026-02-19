#pragma once
#include "../tool.hpp"

namespace ptrclaw {

class FileReadTool : public Tool {
public:
    ToolResult execute(const std::string& args_json) override;
    std::string tool_name() const override { return "file_read"; }
    std::string description() const override;
    std::string parameters_json() const override;
};

} // namespace ptrclaw
