#include "file_read.hpp"
#include "tool_util.hpp"
#include "../plugin.hpp"
#include <fstream>
#include <sstream>

static ptrclaw::ToolRegistrar reg_file_read("file_read",
    []() { return std::make_unique<ptrclaw::FileReadTool>(); });

namespace ptrclaw {

ToolResult FileReadTool::execute(const std::string& args_json) {
    nlohmann::json args;
    if (auto err = parse_tool_json(args_json, args)) return *err;
    if (auto err = require_string(args, "path")) return *err;

    std::string path = args["path"].get<std::string>();
    if (auto err = validate_safe_path(path)) return *err;

    std::ifstream file(path);
    if (!file.is_open()) {
        return ToolResult{false, "Failed to open file: " + path};
    }

    std::ostringstream ss;
    ss << file.rdbuf();
    std::string contents = ss.str();

    constexpr size_t max_size = 50000;
    if (contents.size() > max_size) {
        contents = contents.substr(0, max_size) + "\n[truncated]";
    }

    return ToolResult{true, contents};
}

std::string FileReadTool::description() const {
    return "Read the contents of a file";
}

std::string FileReadTool::parameters_json() const {
    return R"({"type":"object","properties":{"path":{"type":"string","description":"The path of the file to read"}},"required":["path"]})";
}

} // namespace ptrclaw
