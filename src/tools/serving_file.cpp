#include "serving_file.hpp"
#include "tool_util.hpp"
#include "../plugin.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>

// Registered only in a serving build. The classes themselves compile everywhere so the
// default test suite exercises them; with no registrar referencing them, LTO drops the
// code from a personal binary.
#ifdef PTRCLAW_HAS_SERVING
static ptrclaw::ToolRegistrar reg_scoped_file_read("file_read",
    []() { return std::make_unique<ptrclaw::ScopedFileReadTool>(); });
static ptrclaw::ToolRegistrar reg_scoped_file_write("file_write",
    []() { return std::make_unique<ptrclaw::ScopedFileWriteTool>(); });
#endif

namespace ptrclaw {

namespace {

// Same refusal for reads and writes, and it has to name the path: a model told only
// "denied" retries the same argument.
ToolResult refused(const std::string& path) {
    return ToolResult{false,
        "Path is outside this session's workspace: " + path};
}

} // namespace

ToolResult ScopedFileReadTool::execute(const std::string& args_json) {
    nlohmann::json args;
    if (auto err = parse_tool_json(args_json, args)) return *err;
    if (auto err = require_string(args, "path")) return *err;

    std::string requested = args["path"].get<std::string>();
    auto resolved = resolve_in_workspace(workspace_, requested, WorkspaceAccess::Read);
    if (!resolved) return refused(requested);

    std::ifstream file(*resolved);
    if (!file.is_open()) {
        return ToolResult{false, "Failed to open file: " + requested};
    }

    std::ostringstream ss;
    ss << file.rdbuf();
    std::string contents = ss.str();

    // Same cap as FileReadTool: the limit is about the context window, not about scope.
    constexpr size_t max_size = 50000;
    if (contents.size() > max_size) {
        contents = contents.substr(0, max_size) + "\n[truncated]";
    }

    return ToolResult{true, contents};
}

std::string ScopedFileReadTool::description() const {
    return "Read the contents of a file";
}

std::string ScopedFileReadTool::parameters_json() const {
    return R"({"type":"object","properties":{"path":{"type":"string","description":"The path of the file to read"}},"required":["path"]})";
}

ToolResult ScopedFileWriteTool::execute(const std::string& args_json) {
    nlohmann::json args;
    if (auto err = parse_tool_json(args_json, args)) return *err;
    if (auto err = require_string(args, "path")) return *err;
    if (auto err = require_string(args, "content")) return *err;

    std::string requested = args["path"].get<std::string>();
    std::string content = args["content"].get<std::string>();

    auto resolved = resolve_in_workspace(workspace_, requested, WorkspaceAccess::Write);
    if (!resolved) return refused(requested);

    // The parent is inside the workspace by construction — the resolved path is, and this
    // is a prefix of it — so creating it cannot reach outside.
    std::filesystem::path fs_path(*resolved);
    if (fs_path.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(fs_path.parent_path(), ec);
        if (ec) {
            return ToolResult{false, "Failed to create directories: " + ec.message()};
        }
    }

    std::ofstream file(*resolved);
    if (!file.is_open()) {
        return ToolResult{false, "Failed to open file for writing: " + requested};
    }

    file << content;
    file.close();

    if (file.fail()) {
        return ToolResult{false, "Failed to write to file: " + requested};
    }

    // The resolved path, not the requested one: a relative argument landed in the
    // session's workspace and the model should learn where.
    return ToolResult{true, "File written: " + *resolved};
}

std::string ScopedFileWriteTool::description() const {
    return "Write content to a file, creating it if it doesn't exist";
}

std::string ScopedFileWriteTool::parameters_json() const {
    return R"({"type":"object","properties":{"path":{"type":"string","description":"The path of the file to write"},"content":{"type":"string","description":"The content to write to the file"}},"required":["path","content"]})";
}

} // namespace ptrclaw
