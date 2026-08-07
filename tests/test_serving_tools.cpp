#include <catch2/catch_test_macros.hpp>
#include "tools/serving_file.hpp"
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <unistd.h>

using namespace ptrclaw;
using json = nlohmann::json;

namespace {

struct ScopeFixture {
    std::filesystem::path root;
    std::filesystem::path workspace;
    std::filesystem::path context;
    std::filesystem::path outside;

    ScopeFixture() {
        root = std::filesystem::temp_directory_path() /
               ("ptrclaw_svc_" + std::to_string(getpid()) + "_" +
                std::to_string(counter()));
        std::filesystem::create_directories(root);
        root = std::filesystem::canonical(root);
        workspace = root / "sessions" / "abcd-sess";
        context = root / "context";
        outside = root / "elsewhere";
        std::filesystem::create_directories(workspace);
        std::filesystem::create_directories(context);
        std::filesystem::create_directories(outside);
        std::ofstream(context / "brief.md") << "the shared brief\n";
        std::ofstream(outside / "secret.txt") << "provider keys\n";
    }

    ~ScopeFixture() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    ScopeFixture(const ScopeFixture&) = delete;
    ScopeFixture& operator=(const ScopeFixture&) = delete;

    SessionWorkspace scope() const {
        return SessionWorkspace{workspace.string(), context.string(),
                                (root / "sessions").string()};
    }

    static int counter() {
        static int n = 0;
        return ++n;
    }
};

std::string read_args(const std::string& path) {
    return json{{"path", path}}.dump();
}

std::string write_args(const std::string& path, const std::string& content) {
    return json{{"path", path}, {"content", content}}.dump();
}

} // namespace

// ── identity ────────────────────────────────────────────────────

// Registered under the same names and schemas as the unscoped tools, so a serving build
// needs no prompt or schema of its own — only the resolution behind them differs.
TEST_CASE("ScopedFileTools: present themselves as file_read and file_write",
          "[serving][tools]") {
    ScopedFileReadTool read;
    ScopedFileWriteTool write;

    REQUIRE(read.tool_name() == "file_read");
    REQUIRE(write.tool_name() == "file_write");

    auto read_schema = json::parse(read.parameters_json());
    REQUIRE(read_schema["required"] == json::array({"path"}));
    auto write_schema = json::parse(write.parameters_json());
    REQUIRE(write_schema["required"] == json::array({"path", "content"}));
}

// ── reads ───────────────────────────────────────────────────────

TEST_CASE("ScopedFileReadTool: reads a relative path from the workspace",
          "[serving][tools]") {
    ScopeFixture fx;
    std::ofstream(fx.workspace / "notes.md") << "mine\n";

    ScopedFileReadTool tool;
    tool.set_workspace(fx.scope());
    auto result = tool.execute(read_args("notes.md"));

    REQUIRE(result.success);
    REQUIRE(result.output == "mine\n");
}

TEST_CASE("ScopedFileReadTool: reads the shared context", "[serving][tools]") {
    ScopeFixture fx;
    ScopedFileReadTool tool;
    tool.set_workspace(fx.scope());

    auto result = tool.execute(read_args((fx.context / "brief.md").string()));
    REQUIRE(result.success);
    REQUIRE(result.output == "the shared brief\n");
}

TEST_CASE("ScopedFileReadTool: refuses a path outside every root", "[serving][tools]") {
    ScopeFixture fx;
    ScopedFileReadTool tool;
    tool.set_workspace(fx.scope());

    auto result = tool.execute(read_args((fx.outside / "secret.txt").string()));
    REQUIRE_FALSE(result.success);
    // The message has to say why, or the model retries the same path forever.
    REQUIRE(result.output.find("outside") != std::string::npos);
    REQUIRE(result.output.find("secret.txt") != std::string::npos);
}

TEST_CASE("ScopedFileReadTool: refuses everything with no scope wired",
          "[serving][tools]") {
    // A tool constructed but never given a scope must fail closed rather than fall back
    // to the process cwd, which is where the pod's own config lives.
    ScopedFileReadTool tool;
    auto result = tool.execute(read_args("/etc/passwd"));
    REQUIRE_FALSE(result.success);
}

TEST_CASE("ScopedFileReadTool: truncates a large file", "[serving][tools]") {
    ScopeFixture fx;
    {
        std::ofstream big(fx.workspace / "big.txt");
        big << std::string(60000, 'x');
    }

    ScopedFileReadTool tool;
    tool.set_workspace(fx.scope());
    auto result = tool.execute(read_args("big.txt"));

    REQUIRE(result.success);
    REQUIRE(result.output.size() < 60000);
    REQUIRE(result.output.find("[truncated]") != std::string::npos);
}

TEST_CASE("ScopedFileReadTool: a missing file inside the workspace reports not found",
          "[serving][tools]") {
    ScopeFixture fx;
    ScopedFileReadTool tool;
    tool.set_workspace(fx.scope());

    auto result = tool.execute(read_args("absent.md"));
    REQUIRE_FALSE(result.success);
    // Distinct from a refusal: the path was allowed, the file simply is not there.
    REQUIRE(result.output.find("outside") == std::string::npos);
}

// ── writes ──────────────────────────────────────────────────────

TEST_CASE("ScopedFileWriteTool: writes into the session workspace", "[serving][tools]") {
    ScopeFixture fx;
    ScopedFileWriteTool tool;
    tool.set_workspace(fx.scope());

    auto result = tool.execute(write_args("out/report.md", "done\n"));
    REQUIRE(result.success);

    std::ifstream written(fx.workspace / "out/report.md");
    std::ostringstream ss;
    ss << written.rdbuf();
    REQUIRE(ss.str() == "done\n");
}

// Read-only shared context is the guarantee that two concurrent tasks cannot clobber the
// files they are both working from.
TEST_CASE("ScopedFileWriteTool: refuses to write into the shared context",
          "[serving][tools]") {
    ScopeFixture fx;
    ScopedFileWriteTool tool;
    tool.set_workspace(fx.scope());

    auto result = tool.execute(write_args((fx.context / "brief.md").string(), "tampered"));
    REQUIRE_FALSE(result.success);
    REQUIRE(result.output.find("read-only") != std::string::npos);

    // And the file is untouched, not merely reported as failed.
    std::ifstream original(fx.context / "brief.md");
    std::ostringstream ss;
    ss << original.rdbuf();
    REQUIRE(ss.str() == "the shared brief\n");
}

TEST_CASE("ScopedFileWriteTool: refuses an absolute path outside the workspace",
          "[serving][tools]") {
    ScopeFixture fx;
    ScopedFileWriteTool tool;
    tool.set_workspace(fx.scope());

    auto result = tool.execute(write_args((fx.outside / "planted.txt").string(), "x"));
    REQUIRE_FALSE(result.success);
    REQUIRE_FALSE(std::filesystem::exists(fx.outside / "planted.txt"));
}

TEST_CASE("ScopedFileWriteTool: refuses traversal out of the workspace",
          "[serving][tools]") {
    ScopeFixture fx;
    ScopedFileWriteTool tool;
    tool.set_workspace(fx.scope());

    auto result = tool.execute(write_args("../../elsewhere/planted.txt", "x"));
    REQUIRE_FALSE(result.success);
    REQUIRE_FALSE(std::filesystem::exists(fx.outside / "planted.txt"));
}

// ── directories and size ────────────────────────────────────────

// ifstream opens a directory on macOS and Linux and yields zero bytes, so without a check
// this reports success with empty content. The profile ships no listing tool, so reading
// the context directory is exactly what a model tries first — and "empty" would tell it no
// context was staged at all.
TEST_CASE("ScopedFileReadTool: reading a directory is an error, not empty success",
          "[serving][tools]") {
    ScopeFixture fx;
    ScopedFileReadTool tool;
    tool.set_workspace(fx.scope());

    auto result = tool.execute(read_args(fx.context.string()));
    REQUIRE_FALSE(result.success);
    REQUIRE(result.output.find("directory") != std::string::npos);
}

// One tenant's oversized file must not be able to allocate its way through a pod every
// other session is sharing, so the cap has to bound the read rather than the result.
TEST_CASE("ScopedFileReadTool: does not allocate beyond the cap", "[serving][tools]") {
    ScopeFixture fx;
    {
        std::ofstream big(fx.workspace / "huge.txt");
        big << std::string(400000, 'x');
    }

    ScopedFileReadTool tool;
    tool.set_workspace(fx.scope());
    auto result = tool.execute(read_args("huge.txt"));

    REQUIRE(result.success);
    // Truncated, and the marker still tells the model the file continues.
    REQUIRE(result.output.size() < 60000);
    REQUIRE(result.output.find("[truncated]") != std::string::npos);
}

// ── refusal messages ────────────────────────────────────────────

// The prompt lists the shared context as one of the session's roots, so "outside this
// session's workspace" for a path in it is actively misleading — the model cannot infer
// that writing into its own directory is the recovery.
TEST_CASE("ScopedFileWriteTool: says the shared context is read-only",
          "[serving][tools]") {
    ScopeFixture fx;
    ScopedFileWriteTool tool;
    tool.set_workspace(fx.scope());

    auto result = tool.execute(write_args((fx.context / "brief.md").string(), "x"));
    REQUIRE_FALSE(result.success);
    REQUIRE(result.output.find("read-only") != std::string::npos);
    // And it must not claim the path is outside the roots, because it is not.
    REQUIRE(result.output.find("outside") == std::string::npos);
}

TEST_CASE("ScopedFileWriteTool: still says outside for a genuinely unreachable path",
          "[serving][tools]") {
    ScopeFixture fx;
    ScopedFileWriteTool tool;
    tool.set_workspace(fx.scope());

    auto result = tool.execute(write_args((fx.outside / "planted.txt").string(), "x"));
    REQUIRE_FALSE(result.success);
    REQUIRE(result.output.find("outside") != std::string::npos);
    REQUIRE(result.output.find("read-only") == std::string::npos);
}

// ── argument handling ───────────────────────────────────────────

TEST_CASE("ScopedFileTools: malformed arguments are reported", "[serving][tools]") {
    ScopeFixture fx;
    ScopedFileReadTool read;
    read.set_workspace(fx.scope());
    REQUIRE_FALSE(read.execute("not json").success);
    REQUIRE_FALSE(read.execute("{}").success);

    ScopedFileWriteTool write;
    write.set_workspace(fx.scope());
    REQUIRE_FALSE(write.execute(read_args("notes.md")).success);  // no content
}
