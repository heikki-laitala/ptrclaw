#include "shell.hpp"
#include "tool_util.hpp"
#include "../plugin.hpp"
#include <array>
#include <csignal>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

static ptrclaw::ToolRegistrar reg_shell("shell",
    []() { return std::make_unique<ptrclaw::ShellTool>(); });

namespace ptrclaw {

ShellTool::~ShellTool() {
    kill_all_processes();
}

void ShellTool::reset() {
    kill_all_processes();
}

ToolResult ShellTool::execute(const std::string& args_json) {
    return execute(args_json, nullptr);
}

ToolResult ShellTool::execute(const std::string& args_json,
                               const CancellationToken& token) {
    nlohmann::json args;
    if (auto err = parse_tool_json(args_json, args)) return *err;

    std::string stdin_data;
    bool has_stdin = args.contains("stdin") && args["stdin"].is_string();
    if (has_stdin) {
        stdin_data = args["stdin"].get<std::string>();
    }

    // Resume existing process (ignore empty string — some clients send all schema fields)
    if (args.contains("process_id") && args["process_id"].is_string()
        && !args["process_id"].get<std::string>().empty()) {
        return resume_process(args["process_id"].get<std::string>(), stdin_data, token);
    }

    // New command
    if (!args.contains("command") || !args["command"].is_string()) {
        return ToolResult{false, "Missing required parameter: command (or process_id to resume)"};
    }

    return run_new_command(args["command"].get<std::string>(), stdin_data, has_stdin, token);
}

ToolResult ShellTool::run_new_command(const std::string& command,
                                     const std::string& stdin_data,
                                     bool has_stdin,
                                     const CancellationToken& token) {
    std::string cmd = command + " 2>&1";

    int stdin_pipe[2];
    int stdout_pipe[2];

    if (pipe(stdin_pipe) != 0 || pipe(stdout_pipe) != 0) {
        return ToolResult{false, "Failed to create pipes"};
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        return ToolResult{false, "Failed to fork process"};
    }

    if (pid == 0) {
        // Child process — detach from controlling terminal
        setsid();
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stdout_pipe[1], STDERR_FILENO);
        close(stdin_pipe[0]);
        close(stdout_pipe[1]);
        execl("/bin/sh", "sh", "-c", cmd.c_str(), nullptr);
        _exit(127);
    }

    // Parent process
    close(stdin_pipe[0]);
    close(stdout_pipe[1]);

    // Write stdin data if provided; close stdin when caller explicitly supplied
    // the parameter (even if empty) so the child gets EOF. When no stdin param
    // was given, leave the pipe open — stall detection will catch commands that
    // block on input and return them as interactive processes.
    if (has_stdin) {
        if (!stdin_data.empty()) {
            size_t written = 0;
            while (written < stdin_data.size()) {
                ssize_t n = write(stdin_pipe[1], stdin_data.data() + written,
                                  stdin_data.size() - written);
                if (n <= 0) break;
                written += static_cast<size_t>(n);
            }
        }
        close(stdin_pipe[1]);
        stdin_pipe[1] = -1;
    }

    // Read output with stall detection
    constexpr size_t max_output = 10000;
    auto result = read_with_timeout(stdout_pipe[0], pid, kStallTimeoutMs, token);

    if (result.output.size() > max_output) {
        result.output = result.output.substr(0, max_output) + "\n[truncated]";
    }

    if (result.cancelled) {
        // Cancelled — kill child and report
        if (stdin_pipe[1] >= 0) close(stdin_pipe[1]);
        kill(pid, SIGKILL);
        close(stdout_pipe[0]);
        int status = 0;
        waitpid(pid, &status, 0);
        return ToolResult{false, result.output + "\n[cancelled]"};
    }

    if (!result.still_running) {
        // Process finished
        if (stdin_pipe[1] >= 0) close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        int status = result.exit_status;
        if (!result.reaped) {
            waitpid(pid, &status, 0);
        }
        bool success = WIFEXITED(status) && WEXITSTATUS(status) == 0;
        return ToolResult{success, result.output};
    }

    // Process is stalled — waiting for input
    // Evict oldest process if at capacity
    while (processes_.size() >= kMaxProcesses) {
        auto oldest = processes_.begin();
        cleanup_process(oldest->first);
    }

    std::string proc_id = "proc_" + std::to_string(next_id_++);
    int stored_stdin = stdin_pipe[1] >= 0 ? stdin_pipe[1] : -1;
    processes_[proc_id] = ProcessState{pid, stored_stdin, stdout_pipe[0]};

    result.output += "\n[WAITING FOR INPUT - process_id:" + proc_id + "]";
    return ToolResult{true, result.output};
}

ToolResult ShellTool::resume_process(const std::string& proc_id,
                                     const std::string& stdin_data,
                                     const CancellationToken& token) {
    auto it = processes_.find(proc_id);
    if (it == processes_.end()) {
        return ToolResult{false, "No such process: " + proc_id};
    }

    auto& proc = it->second;

    // Write stdin data
    if (!stdin_data.empty() && proc.stdin_fd >= 0) {
        std::string data = stdin_data;
        if (data.back() != '\n') {
            data += '\n';
        }
        size_t written = 0;
        while (written < data.size()) {
            ssize_t n = write(proc.stdin_fd, data.data() + written,
                              data.size() - written);
            if (n <= 0) break;
            written += static_cast<size_t>(n);
        }
    }

    // Read new output — use longer timeout since we just sent data and
    // the process may need time for network/IO before responding
    constexpr size_t max_output = 10000;
    auto result = read_with_timeout(proc.stdout_fd, proc.pid, kResumeTimeoutMs, token);

    if (result.output.size() > max_output) {
        result.output = result.output.substr(0, max_output) + "\n[truncated]";
    }

    if (result.cancelled) {
        cleanup_process(proc_id);
        return ToolResult{false, result.output + "\n[cancelled]"};
    }

    if (!result.still_running) {
        if (proc.stdin_fd >= 0) close(proc.stdin_fd);
        close(proc.stdout_fd);
        int status = result.exit_status;
        if (!result.reaped) {
            waitpid(proc.pid, &status, 0);
        }
        processes_.erase(it);
        bool success = WIFEXITED(status) && WEXITSTATUS(status) == 0;
        return ToolResult{success, result.output};
    }

    // Still waiting
    result.output += "\n[WAITING FOR INPUT - process_id:" + proc_id + "]";
    return ToolResult{true, result.output};
}

ShellTool::ReadResult ShellTool::read_with_timeout(int stdout_fd, pid_t pid,
                                                    int timeout_ms,
                                                    const CancellationToken& token) {
    std::string output;
    std::array<char, 4096> buffer;
    // Use short poll intervals to check cancellation, but track total stall time
    constexpr int kPollIntervalMs = 200;

    int stall_elapsed_ms = 0;

    while (true) {
        if (is_cancelled(token)) {
            return {output, true, true, 0, false};
        }

        struct pollfd pfd;
        pfd.fd = stdout_fd;
        pfd.events = POLLIN;

        int poll_ms = (timeout_ms > 0)
            ? std::min(kPollIntervalMs, timeout_ms - stall_elapsed_ms)
            : kPollIntervalMs;
        if (poll_ms <= 0) poll_ms = 1;

        int ret = poll(&pfd, 1, poll_ms);

        if (ret < 0) {
            break; // poll error
        }

        if (ret == 0) {
            stall_elapsed_ms += poll_ms;
            // Check cancellation on every poll timeout
            if (is_cancelled(token)) {
                return {output, true, true, 0, false};
            }
            // Check if we've exceeded the stall timeout
            if (timeout_ms > 0 && stall_elapsed_ms >= timeout_ms) {
                int status = 0;
                pid_t result = waitpid(pid, &status, WNOHANG);
                if (result == 0) {
                    return {output, true, false, 0, false};
                }
                return {output, false, false, status, result > 0};
            }
            continue;
        }

        // Got activity — reset stall timer
        stall_elapsed_ms = 0;

        if ((pfd.revents & POLLIN) != 0) {
            ssize_t n = read(stdout_fd, buffer.data(), buffer.size());
            if (n > 0) {
                output.append(buffer.data(), static_cast<size_t>(n));
                continue;
            }
            return {output, false};
        }

        if ((pfd.revents & (POLLHUP | POLLERR)) != 0) {
            return {output, false};
        }
    }

    return {output, false};
}

void ShellTool::cleanup_process(const std::string& id) {
    auto it = processes_.find(id);
    if (it == processes_.end()) return;

    auto& proc = it->second;
    kill(proc.pid, SIGKILL);
    if (proc.stdin_fd >= 0) close(proc.stdin_fd);
    close(proc.stdout_fd);
    int status = 0;
    waitpid(proc.pid, &status, 0);
    processes_.erase(it);
}

void ShellTool::kill_all_processes() {
    for (auto& [id, proc] : processes_) {
        kill(proc.pid, SIGKILL);
        if (proc.stdin_fd >= 0) close(proc.stdin_fd);
        close(proc.stdout_fd);
        int status = 0;
        waitpid(proc.pid, &status, 0);
    }
    processes_.clear();
}

std::string ShellTool::description() const {
    return "Execute a shell command. For interactive commands that wait for input, "
           "the tool returns partial output with a process_id. Use process_id with "
           "stdin to send follow-up input to the waiting process.";
}

std::string ShellTool::parameters_json() const {
    return R"json({"type":"object","properties":{"command":{"type":"string","description":"The shell command to execute (required for new commands)"},"stdin":{"type":"string","description":"Input to write to the command's stdin. For new commands, this is initial input. For resumed processes, this is follow-up input (newline appended automatically)."},"process_id":{"type":"string","description":"Resume a waiting process by its ID. When provided, command is not needed - only stdin is sent to the existing process."}}})json";
}

} // namespace ptrclaw
