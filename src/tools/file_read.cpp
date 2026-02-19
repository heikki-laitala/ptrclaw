#include "file_read.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>

namespace ptrclaw {

ToolResult FileReadTool::execute(const std::string& args_json) {
    nlohmann::json args;
    try {
        args = nlohmann::json::parse(args_json);
    } catch (const std::exception& e) {
        return ToolResult{false, std::string("Failed to parse arguments: ") + e.what()};
    }

    if (!args.contains("path") || !args["path"].is_string()) {
        return ToolResult{false, "Missing required parameter: path"};
    }

    std::string path = args["path"].get<std::string>();

    if (path.find("..") != std::string::npos) {
        return ToolResult{false, "Path must not contain '..'"};
    }

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
