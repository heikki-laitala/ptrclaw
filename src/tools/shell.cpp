#include "shell.hpp"
#include "../plugin.hpp"
#include <nlohmann/json.hpp>
#include <cstdio>
#include <array>
#include <sys/wait.h>

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

    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        return ToolResult{false, "Failed to execute command"};
    }

    std::string output;
    std::array<char, 4096> buffer;
    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        output += buffer.data();
    }

    int status = pclose(pipe);
    bool success = WEXITSTATUS(status) == 0;

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
    return R"({"type":"object","properties":{"command":{"type":"string","description":"The shell command to execute"}},"required":["command"]})";
}

} // namespace ptrclaw
