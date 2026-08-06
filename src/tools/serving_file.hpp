#pragma once
#include "../tool.hpp"

namespace ptrclaw {

// File tools for a pod serving many sessions at once.
//
// They carry the same names and schemas as FileReadTool and FileWriteTool — the model sees
// no difference — but every path is resolved against the calling session's roots by
// resolve_in_workspace(): reads inside the session's workspace or the shared context
// directory, writes inside the workspace only.
//
// Registered instead of the unscoped tools, never alongside them: PluginRegistry keys
// factories by name, so two registrations of "file_read" would leave static-init order to
// decide which one a session gets. meson.build refuses that combination.
class ScopedFileReadTool : public WorkspaceAwareTool {
public:
    ToolResult execute(const std::string& args_json) override;
    std::string tool_name() const override { return "file_read"; }
    std::string description() const override;
    std::string parameters_json() const override;
};

class ScopedFileWriteTool : public WorkspaceAwareTool {
public:
    ToolResult execute(const std::string& args_json) override;
    std::string tool_name() const override { return "file_write"; }
    std::string description() const override;
    std::string parameters_json() const override;
};

} // namespace ptrclaw
