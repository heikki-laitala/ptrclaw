#include "file_edit.hpp"
#include "../plugin.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>

static ptrclaw::ToolRegistrar reg_file_edit("file_edit",
    []() { return std::make_unique<ptrclaw::FileEditTool>(); });

namespace ptrclaw {

ToolResult FileEditTool::execute(const std::string& args_json) {
    nlohmann::json args;
    try {
        args = nlohmann::json::parse(args_json);
    } catch (const std::exception& e) {
        return ToolResult{false, std::string("Failed to parse arguments: ") + e.what()};
    }

    if (!args.contains("path") || !args["path"].is_string()) {
        return ToolResult{false, "Missing required parameter: path"};
    }
    if (!args.contains("old_text") || !args["old_text"].is_string()) {
        return ToolResult{false, "Missing required parameter: old_text"};
    }
    if (!args.contains("new_text") || !args["new_text"].is_string()) {
        return ToolResult{false, "Missing required parameter: new_text"};
    }

    std::string path = args["path"].get<std::string>();
    std::string old_text = args["old_text"].get<std::string>();
    std::string new_text = args["new_text"].get<std::string>();

    if (path.find("..") != std::string::npos) {
        return ToolResult{false, "Path must not contain '..'"};
    }

    // Read file
    std::ifstream infile(path);
    if (!infile.is_open()) {
        return ToolResult{false, "Failed to open file: " + path};
    }

    std::ostringstream ss;
    ss << infile.rdbuf();
    std::string contents = ss.str();
    infile.close();

    // Find old_text
    size_t first_pos = contents.find(old_text);
    if (first_pos == std::string::npos) {
        return ToolResult{false, "old_text not found in file"};
    }

    // Check for ambiguity
    size_t second_pos = contents.find(old_text, first_pos + 1);
    if (second_pos != std::string::npos) {
        return ToolResult{false, "old_text found multiple times in file (ambiguous edit)"};
    }

    // Replace
    contents.replace(first_pos, old_text.size(), new_text);

    // Write back
    std::ofstream outfile(path);
    if (!outfile.is_open()) {
        return ToolResult{false, "Failed to open file for writing: " + path};
    }

    outfile << contents;
    outfile.close();

    if (outfile.fail()) {
        return ToolResult{false, "Failed to write to file: " + path};
    }

    return ToolResult{true, "File edited: " + path};
}

std::string FileEditTool::description() const {
    return "Edit a file by replacing exact text";
}

std::string FileEditTool::parameters_json() const {
    return R"({"type":"object","properties":{"path":{"type":"string","description":"The path of the file to edit"},"old_text":{"type":"string","description":"The exact text to find and replace"},"new_text":{"type":"string","description":"The replacement text"}},"required":["path","old_text","new_text"]})";
}

} // namespace ptrclaw
