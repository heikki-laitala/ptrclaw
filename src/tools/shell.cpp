#include "shell.hpp"
#include "../plugin.hpp"
#include <nlohmann/json.hpp>
#include <array>
#include <sys/wait.h>
#include <unistd.h>

static ptrclaw::ToolRegistrar reg_shell("shell",
    []() { return std::make_unique<ptrclaw::ShellTool>(); });

namespace ptrclaw {

ToolResult ShellTool::execute(const std::string& args_json) {
    nlohmann::json args;
    try {
        args = nlohmann::json::parse(args_json);
    } catch (const std::exception& e) {
        return ToolResult{false, std::string("Failed to parse arguments: ") + e.what()};
    }

    if (!args.contains("command") || !args["command"].is_string()) {
        return ToolResult{false, "Missing required parameter: command"};
    }

    std::string command = args["command"].get<std::string>() + " 2>&1";

    std::string stdin_data;
    bool has_stdin = args.contains("stdin") && args["stdin"].is_string();
    if (has_stdin) {
        stdin_data = args["stdin"].get<std::string>();
    }

    // stdin pipe: parent writes to stdin_pipe[1], child reads from stdin_pipe[0]
    // stdout pipe: child writes to stdout_pipe[1], parent reads from stdout_pipe[0]
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
        // Child process
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stdout_pipe[1], STDERR_FILENO);
        close(stdin_pipe[0]);
        close(stdout_pipe[1]);
        execl("/bin/sh", "sh", "-c", command.c_str(), nullptr);
        _exit(127);
    }

    // Parent process
    close(stdin_pipe[0]);
    close(stdout_pipe[1]);

    if (has_stdin && !stdin_data.empty()) {
        // Write all stdin data; ignore partial-write edge cases for typical payloads
        size_t written = 0;
        while (written < stdin_data.size()) {
            ssize_t n = write(stdin_pipe[1], stdin_data.data() + written,
                              stdin_data.size() - written);
            if (n <= 0) break;
            written += static_cast<size_t>(n);
        }
    }
    close(stdin_pipe[1]); // Send EOF to child

    std::string output;
    std::array<char, 4096> buffer;
    ssize_t n;
    while ((n = read(stdout_pipe[0], buffer.data(), buffer.size())) > 0) {
        output.append(buffer.data(), static_cast<size_t>(n));
    }
    close(stdout_pipe[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    bool success = WIFEXITED(status) && WEXITSTATUS(status) == 0;

    constexpr size_t max_output = 10000;
    if (output.size() > max_output) {
        output = output.substr(0, max_output) + "\n[truncated]";
    }

    return ToolResult{success, output};
}

std::string ShellTool::description() const {
    return "Execute a shell command and return its output";
}

std::string ShellTool::parameters_json() const {
    return R"({"type":"object","properties":{"command":{"type":"string","description":"The shell command to execute"},"stdin":{"type":"string","description":"Optional input to write to the command's stdin. Use with commands that read from stdin (e.g. sudo -S)."}},"required":["command"]})";
}

} // namespace ptrclaw
