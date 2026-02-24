#pragma once
#include "../tool.hpp"
#include <string>

namespace ptrclaw {

class CronTool : public Tool {
public:
    ToolResult execute(const std::string& args_json) override;
    std::string tool_name() const override { return "cron"; }
    std::string description() const override;
    std::string parameters_json() const override;

private:
    ToolResult list_entries();
    ToolResult add_entry(const std::string& schedule, const std::string& command,
                         const std::string& label);
    ToolResult remove_entry(const std::string& label);

    static std::string read_crontab();
    static bool write_crontab(const std::string& contents);
    static bool validate_schedule(const std::string& schedule);
};

} // namespace ptrclaw
