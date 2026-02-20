#include "file_write.hpp"
#include "../plugin.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>

static ptrclaw::ToolRegistrar reg_file_write("file_write",
    []() { return std::make_unique<ptrclaw::FileWriteTool>(); });

namespace ptrclaw {

ToolResult FileWriteTool::execute(const std::string& args_json) {
    nlohmann::json args;
    try {
        args = nlohmann::json::parse(args_json);
    } catch (const std::exception& e) {
        return ToolResult{false, std::string("Failed to parse arguments: ") + e.what()};
    }

    if (!args.contains("path") || !args["path"].is_string()) {
        return ToolResult{false, "Missing required parameter: path"};
    }
    if (!args.contains("content") || !args["content"].is_string()) {
        return ToolResult{false, "Missing required parameter: content"};
    }

    std::string path = args["path"].get<std::string>();
    std::string content = args["content"].get<std::string>();

    if (path.find("..") != std::string::npos) {
        return ToolResult{false, "Path must not contain '..'"};
    }

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
