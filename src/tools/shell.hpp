#pragma once
#include "../tool.hpp"
#include <string>
#include <unordered_map>
#include <sys/types.h>

namespace ptrclaw {

struct ProcessState {
    pid_t pid;
    int stdin_fd;
    int stdout_fd;
};

class ShellTool : public Tool {
public:
    ~ShellTool() override;

    ToolResult execute(const std::string& args_json) override;
    std::string tool_name() const override { return "shell"; }
    std::string description() const override;
    std::string parameters_json() const override;
    void reset() override;

private:
    static constexpr int kStallTimeoutMs = 3000;
    static constexpr size_t kMaxProcesses = 4;

    ToolResult run_new_command(const std::string& command, const std::string& stdin_data,
                              bool has_stdin);
    ToolResult resume_process(const std::string& proc_id, const std::string& stdin_data);

    struct ReadResult {
        std::string output;
        bool still_running;
    };
    ReadResult read_with_timeout(int stdout_fd, pid_t pid);

    void cleanup_process(const std::string& id);
    void kill_all_processes();

    std::unordered_map<std::string, ProcessState> processes_;
    uint32_t next_id_ = 0;
};

} // namespace ptrclaw
