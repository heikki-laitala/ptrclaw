#include "file_write.hpp"
#include "tool_util.hpp"
#include "../plugin.hpp"
#include <fstream>
#include <filesystem>

static ptrclaw::ToolRegistrar reg_file_write("file_write",
    []() { return std::make_unique<ptrclaw::FileWriteTool>(); });

namespace ptrclaw {

ToolResult FileWriteTool::execute(const std::string& args_json) {
    nlohmann::json args;
    if (auto err = parse_tool_json(args_json, args)) return *err;
    if (auto err = require_string(args, "path")) return *err;
    if (auto err = require_string(args, "content")) return *err;

    std::string path = args["path"].get<std::string>();
    std::string content = args["content"].get<std::string>();
    if (auto err = validate_safe_path(path)) return *err;

    std::filesystem::path fs_path(path);
    if (fs_path.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(fs_path.parent_path(), ec);
        if (ec) {
            return ToolResult{false, "Failed to create directories: " + ec.message()};
        }
    }

    std::ofstream file(path);
    if (!file.is_open()) {
        return ToolResult{false, "Failed to open file for writing: " + path};
    }

    file << content;
    file.close();

    if (file.fail()) {
        return ToolResult{false, "Failed to write to file: " + path};
    }

    return ToolResult{true, "File written: " + path};
}

std::string FileWriteTool::description() const {
    return "Write content to a file, creating it if it doesn't exist";
}

std::string FileWriteTool::parameters_json() const {
    return R"({"type":"object","properties":{"path":{"type":"string","description":"The path of the file to write"},"content":{"type":"string","description":"The content to write to the file"}},"required":["path","content"]})";
}

} // namespace ptrclaw
