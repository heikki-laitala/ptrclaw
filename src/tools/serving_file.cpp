#include "serving_file.hpp"
#include "tool_util.hpp"
#include "../plugin.hpp"
#include <filesystem>
#include <fstream>

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

// The refusal has to name both the path and the reason. The system prompt lists the shared
// context as one of the session's roots, so reporting a write into it as "outside the
// workspace" would contradict the prompt and leave the model with no way to work out that
// writing into its own directory is the recovery.
ToolResult refused(const SessionWorkspace& scope, const std::string& path,
                   WorkspaceAccess access) {
    if (access == WorkspaceAccess::Write &&
        resolve_in_workspace(scope, path, WorkspaceAccess::Read)) {
        return ToolResult{false,
            "The shared context directory is read-only. Write inside this session's "
            "workspace instead: " + path};
    }
    return ToolResult{false,
        "Path is outside this session's workspace: " + path};
}

// Read at most one byte past the cap: enough to know the file continues, without letting a
// single oversized file in the shared context allocate its way through a pod that every
// other session is sharing.
constexpr size_t kMaxReadBytes = 50000;

} // namespace

ToolResult ScopedFileReadTool::execute(const std::string& args_json) {
    nlohmann::json args;
    if (auto err = parse_tool_json(args_json, args)) return *err;
    if (auto err = require_string(args, "path")) return *err;

    std::string requested = args["path"].get<std::string>();
    auto resolved = resolve_in_workspace(workspace_, requested, WorkspaceAccess::Read);
    if (!resolved) return refused(workspace_, requested, WorkspaceAccess::Read);

    // Before opening: ifstream opens a directory on macOS and Linux and then reads zero
    // bytes, which would report success with empty content. This profile ships no listing
    // tool, so reading the context directory is the first thing a model tries, and "empty"
    // would tell it nothing was staged.
    std::error_code ec;
    if (std::filesystem::is_directory(*resolved, ec)) {
        return ToolResult{false, "Path is a directory, not a file: " + requested};
    }

    std::ifstream file(*resolved);
    if (!file.is_open()) {
        return ToolResult{false, "Failed to open file: " + requested};
    }

    std::string contents(kMaxReadBytes + 1, '\0');
    file.read(&contents[0], static_cast<std::streamsize>(contents.size()));
    contents.resize(static_cast<size_t>(file.gcount()));

    if (contents.size() > kMaxReadBytes) {
        contents.resize(kMaxReadBytes);
        contents += "\n[truncated]";
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
    if (!resolved) return refused(workspace_, requested, WorkspaceAccess::Write);

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
