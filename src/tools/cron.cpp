#include "cron.hpp"
#include "../plugin.hpp"
#include <nlohmann/json.hpp>
#include <array>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>

static ptrclaw::ToolRegistrar reg_cron("cron",
    []() { return std::make_unique<ptrclaw::CronTool>(); });

namespace ptrclaw {

static const std::string kMarkerPrefix = "# ptrclaw:";

ToolResult CronTool::execute(const std::string& args_json) {
    nlohmann::json args;
    try {
        args = nlohmann::json::parse(args_json);
    } catch (const std::exception& e) {
        return ToolResult{false, std::string("Failed to parse arguments: ") + e.what()};
    }

    if (!args.contains("action") || !args["action"].is_string()) {
        return ToolResult{false, "Missing required parameter: action (list, add, remove)"};
    }

    std::string action = args["action"].get<std::string>();

    if (action == "list") {
        return list_entries();
    }

    if (action == "add") {
        if (!args.contains("schedule") || !args["schedule"].is_string()) {
            return ToolResult{false, "Missing required parameter: schedule"};
        }
        if (!args.contains("command") || !args["command"].is_string()) {
            return ToolResult{false, "Missing required parameter: command"};
        }
        if (!args.contains("label") || !args["label"].is_string()) {
            return ToolResult{false, "Missing required parameter: label"};
        }
        return add_entry(args["schedule"].get<std::string>(),
                         args["command"].get<std::string>(),
                         args["label"].get<std::string>());
    }

    if (action == "remove") {
        if (!args.contains("label") || !args["label"].is_string()) {
            return ToolResult{false, "Missing required parameter: label"};
        }
        return remove_entry(args["label"].get<std::string>());
    }

    return ToolResult{false, "Unknown action: " + action + " (expected: list, add, remove)"};
}

ToolResult CronTool::list_entries() {
    std::string crontab = read_crontab();
    if (crontab.empty()) {
        return ToolResult{true, "(no crontab entries)"};
    }
    return ToolResult{true, crontab};
}

ToolResult CronTool::add_entry(const std::string& schedule, const std::string& command,
                                const std::string& label) {
    if (!validate_schedule(schedule)) {
        return ToolResult{false, "Invalid cron schedule: " + schedule +
            " (must be 5 fields, each containing only 0-9 * / - ,)"};
    }

    if (label.empty()) {
        return ToolResult{false, "Label must not be empty"};
    }

    std::string crontab = read_crontab();

    // Check for duplicate label
    std::string marker = kMarkerPrefix + label;
    if (crontab.find(marker) != std::string::npos) {
        return ToolResult{false, "Label already exists: " + label};
    }

    // Append entry
    if (!crontab.empty() && crontab.back() != '\n') {
        crontab += '\n';
    }
    crontab += marker + '\n';
    crontab += schedule + ' ' + command + '\n';

    if (!write_crontab(crontab)) {
        return ToolResult{false, "Failed to write crontab"};
    }
    return ToolResult{true, "Added cron entry: " + label};
}

ToolResult CronTool::remove_entry(const std::string& label) {
    std::string crontab = read_crontab();
    std::string marker = kMarkerPrefix + label;

    // Find and remove the marker line + the following command line
    std::istringstream stream(crontab);
    std::ostringstream result;
    std::string line;
    bool found = false;
    bool skip_next = false;

    while (std::getline(stream, line)) {
        if (skip_next) {
            skip_next = false;
            continue;
        }
        if (line == marker) {
            found = true;
            skip_next = true;
            continue;
        }
        result << line << '\n';
    }

    if (!found) {
        return ToolResult{false, "No ptrclaw entry with label: " + label};
    }

    if (!write_crontab(result.str())) {
        return ToolResult{false, "Failed to write crontab"};
    }
    return ToolResult{true, "Removed cron entry: " + label};
}

std::string CronTool::read_crontab() {
    // NOLINTNEXTLINE(cert-env33-c)
    FILE* pipe = popen("crontab -l 2>/dev/null", "r");
    if (!pipe) return "";

    std::string output;
    std::array<char, 4096> buffer;
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        output += buffer.data();
    }
    pclose(pipe);
    return output;
}

bool CronTool::write_crontab(const std::string& contents) {
    int pipefd[2];
    if (pipe(pipefd) != 0) return false;

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return false;
    }

    if (pid == 0) {
        // Child: read from pipe, exec crontab -
        close(pipefd[1]);
        dup2(pipefd[0], STDIN_FILENO);
        close(pipefd[0]);
        execlp("crontab", "crontab", "-", nullptr);
        _exit(127);
    }

    // Parent: write contents to pipe
    close(pipefd[0]);
    size_t written = 0;
    while (written < contents.size()) {
        ssize_t n = write(pipefd[1], contents.data() + written,
                          contents.size() - written);
        if (n <= 0) break;
        written += static_cast<size_t>(n);
    }
    close(pipefd[1]);

    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool CronTool::validate_schedule(const std::string& schedule) {
    // Split by whitespace, expect exactly 5 fields
    std::istringstream stream(schedule);
    std::vector<std::string> fields;
    std::string field;
    while (stream >> field) {
        fields.push_back(field);
    }

    if (fields.size() != 5) return false;

    // Each field must contain only [0-9 * / - ,]
    for (const auto& f : fields) {
        if (f.empty()) return false;
        for (char c : f) {
            if (!((c >= '0' && c <= '9') || c == '*' || c == '/' ||
                  c == '-' || c == ',')) {
                return false;
            }
        }
    }
    return true;
}

std::string CronTool::description() const {
    return "Manage scheduled tasks via system crontab. "
           "Actions: list (show all entries), add (schedule+command+label), "
           "remove (by label). Only manages ptrclaw-tagged entries.";
}

std::string CronTool::parameters_json() const {
    return R"json({"type":"object","properties":{"action":{"type":"string","description":"Action to perform: list, add, or remove","enum":["list","add","remove"]},"schedule":{"type":"string","description":"Cron schedule expression (5 fields: minute hour day month weekday). Required for add."},"command":{"type":"string","description":"Shell command to execute on schedule. Required for add."},"label":{"type":"string","description":"Unique label for the cron entry. Required for add and remove."}},"required":["action"]})json";
}

} // namespace ptrclaw
