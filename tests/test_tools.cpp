#include <catch2/catch_test_macros.hpp>
#include "tools/file_read.hpp"
#include "tools/file_write.hpp"
#include "tools/file_edit.hpp"
#include "tools/shell.hpp"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <cstdlib>
#include <unistd.h>

using namespace ptrclaw;

// Helper: create a temp directory and return its path
static std::string make_temp_dir() {
    auto path = std::filesystem::temp_directory_path() / "ptrclaw_test_XXXXXX";
    std::string tmpl = path.string();
    // Use mkdtemp for a unique directory
    char* result = mkdtemp(tmpl.data());
    return result ? std::string(result) : "";
}

// Helper: write a file directly for setup
static void write_file(const std::string& path, const std::string& content) {
    std::ofstream f(path);
    f << content;
}

// Helper: read a file directly for verification
static std::string read_file(const std::string& path) {
    std::ifstream f(path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// ═══ FileReadTool ════════════════════════════════════════════════

TEST_CASE("FileReadTool: reads existing file", "[tools]") {
    auto dir = make_temp_dir();
    REQUIRE_FALSE(dir.empty());
    auto file = dir + "/test.txt";
    write_file(file, "hello world");

    FileReadTool tool;
    auto result = tool.execute(R"({"path":")" + file + R"("})");
    REQUIRE(result.success);
    REQUIRE(result.output == "hello world");

    std::filesystem::remove_all(dir);
}

TEST_CASE("FileReadTool: missing path parameter", "[tools]") {
    FileReadTool tool;
    auto result = tool.execute(R"({})");
    REQUIRE_FALSE(result.success);
    REQUIRE(result.output.find("path") != std::string::npos);
}

TEST_CASE("FileReadTool: nonexistent file", "[tools]") {
    FileReadTool tool;
    auto result = tool.execute(R"({"path":"/tmp/ptrclaw_test_no_such_file_ever.txt"})");
    REQUIRE_FALSE(result.success);
    REQUIRE(result.output.find("Failed to open") != std::string::npos);
}

TEST_CASE("FileReadTool: rejects path traversal", "[tools]") {
    FileReadTool tool;
    auto result = tool.execute(R"({"path":"../../../etc/passwd"})");
    REQUIRE_FALSE(result.success);
    REQUIRE(result.output.find("..") != std::string::npos);
}

TEST_CASE("FileReadTool: invalid JSON args", "[tools]") {
    FileReadTool tool;
    auto result = tool.execute("not json");
    REQUIRE_FALSE(result.success);
    REQUIRE(result.output.find("parse") != std::string::npos);
}

TEST_CASE("FileReadTool: tool_name is file_read", "[tools]") {
    FileReadTool tool;
    REQUIRE(tool.tool_name() == "file_read");
}

// ═══ FileWriteTool ═══════════════════════════════════════════════

TEST_CASE("FileWriteTool: writes new file", "[tools]") {
    auto dir = make_temp_dir();
    REQUIRE_FALSE(dir.empty());
    auto file = dir + "/output.txt";

    FileWriteTool tool;
    auto result = tool.execute(R"({"path":")" + file + R"(","content":"written content"})");
    REQUIRE(result.success);
    REQUIRE(read_file(file) == "written content");

    std::filesystem::remove_all(dir);
}

TEST_CASE("FileWriteTool: creates parent directories", "[tools]") {
    auto dir = make_temp_dir();
    REQUIRE_FALSE(dir.empty());
    auto file = dir + "/sub/deep/file.txt";

    FileWriteTool tool;
    auto result = tool.execute(R"({"path":")" + file + R"(","content":"nested"})");
    REQUIRE(result.success);
    REQUIRE(read_file(file) == "nested");

    std::filesystem::remove_all(dir);
}

TEST_CASE("FileWriteTool: missing content parameter", "[tools]") {
    FileWriteTool tool;
    auto result = tool.execute(R"({"path":"/tmp/test.txt"})");
    REQUIRE_FALSE(result.success);
    REQUIRE(result.output.find("content") != std::string::npos);
}

TEST_CASE("FileWriteTool: rejects path traversal", "[tools]") {
    FileWriteTool tool;
    auto result = tool.execute(R"({"path":"../bad.txt","content":"x"})");
    REQUIRE_FALSE(result.success);
}

TEST_CASE("FileWriteTool: invalid JSON args", "[tools]") {
    FileWriteTool tool;
    auto result = tool.execute("not json");
    REQUIRE_FALSE(result.success);
    REQUIRE(result.output.find("parse") != std::string::npos);
}

TEST_CASE("FileWriteTool: missing path parameter", "[tools]") {
    FileWriteTool tool;
    auto result = tool.execute(R"({"content":"x"})");
    REQUIRE_FALSE(result.success);
    REQUIRE(result.output.find("path") != std::string::npos);
}

TEST_CASE("FileWriteTool: tool_name is file_write", "[tools]") {
    FileWriteTool tool;
    REQUIRE(tool.tool_name() == "file_write");
    REQUIRE_FALSE(tool.description().empty());
    REQUIRE(tool.parameters_json().find("path") != std::string::npos);
}

// ═══ FileEditTool ════════════════════════════════════════════════

TEST_CASE("FileEditTool: replaces text in file", "[tools]") {
    auto dir = make_temp_dir();
    REQUIRE_FALSE(dir.empty());
    auto file = dir + "/edit.txt";
    write_file(file, "hello world");

    FileEditTool tool;
    auto result = tool.execute(R"({"path":")" + file + R"(","old_text":"world","new_text":"there"})");
    REQUIRE(result.success);
    REQUIRE(read_file(file) == "hello there");

    std::filesystem::remove_all(dir);
}

TEST_CASE("FileEditTool: fails on ambiguous match", "[tools]") {
    auto dir = make_temp_dir();
    REQUIRE_FALSE(dir.empty());
    auto file = dir + "/dup.txt";
    write_file(file, "aaa bbb aaa");

    FileEditTool tool;
    auto result = tool.execute(R"({"path":")" + file + R"(","old_text":"aaa","new_text":"ccc"})");
    REQUIRE_FALSE(result.success);
    REQUIRE(result.output.find("multiple") != std::string::npos);

    std::filesystem::remove_all(dir);
}

TEST_CASE("FileEditTool: fails when old_text not found", "[tools]") {
    auto dir = make_temp_dir();
    REQUIRE_FALSE(dir.empty());
    auto file = dir + "/miss.txt";
    write_file(file, "hello");

    FileEditTool tool;
    auto result = tool.execute(R"({"path":")" + file + R"(","old_text":"xyz","new_text":"abc"})");
    REQUIRE_FALSE(result.success);
    REQUIRE(result.output.find("not found") != std::string::npos);

    std::filesystem::remove_all(dir);
}

TEST_CASE("FileEditTool: missing old_text parameter", "[tools]") {
    FileEditTool tool;
    auto result = tool.execute(R"({"path":"/tmp/x","new_text":"y"})");
    REQUIRE_FALSE(result.success);
    REQUIRE(result.output.find("old_text") != std::string::npos);
}

TEST_CASE("FileEditTool: rejects path traversal", "[tools]") {
    FileEditTool tool;
    auto result = tool.execute(R"({"path":"../../x","old_text":"a","new_text":"b"})");
    REQUIRE_FALSE(result.success);
}

TEST_CASE("FileEditTool: invalid JSON args", "[tools]") {
    FileEditTool tool;
    auto result = tool.execute("not json");
    REQUIRE_FALSE(result.success);
    REQUIRE(result.output.find("parse") != std::string::npos);
}

TEST_CASE("FileEditTool: missing new_text parameter", "[tools]") {
    FileEditTool tool;
    auto result = tool.execute(R"({"path":"/tmp/x","old_text":"a"})");
    REQUIRE_FALSE(result.success);
    REQUIRE(result.output.find("new_text") != std::string::npos);
}

TEST_CASE("FileEditTool: nonexistent file", "[tools]") {
    FileEditTool tool;
    auto result = tool.execute(R"({"path":"/tmp/ptrclaw_no_such_file.txt","old_text":"a","new_text":"b"})");
    REQUIRE_FALSE(result.success);
    REQUIRE(result.output.find("Failed to open") != std::string::npos);
}

TEST_CASE("FileEditTool: tool_name is file_edit", "[tools]") {
    FileEditTool tool;
    REQUIRE(tool.tool_name() == "file_edit");
    REQUIRE_FALSE(tool.description().empty());
    REQUIRE(tool.parameters_json().find("old_text") != std::string::npos);
}

// ═══ ShellTool ═══════════════════════════════════════════════════

TEST_CASE("ShellTool: runs simple command", "[tools]") {
    ShellTool tool;
    auto result = tool.execute(R"({"command":"echo hello"})");
    REQUIRE(result.success);
    REQUIRE(result.output.find("hello") != std::string::npos);
}

TEST_CASE("ShellTool: captures exit code failure", "[tools]") {
    ShellTool tool;
    auto result = tool.execute(R"({"command":"false"})");
    REQUIRE_FALSE(result.success);
}

TEST_CASE("ShellTool: missing command parameter", "[tools]") {
    ShellTool tool;
    auto result = tool.execute(R"({})");
    REQUIRE_FALSE(result.success);
    REQUIRE(result.output.find("command") != std::string::npos);
}

TEST_CASE("ShellTool: invalid JSON args", "[tools]") {
    ShellTool tool;
    auto result = tool.execute("garbage");
    REQUIRE_FALSE(result.success);
}

TEST_CASE("ShellTool: captures stderr via redirect", "[tools]") {
    ShellTool tool;
    // The tool appends 2>&1 to the command, so use a command that writes to stderr
    // via a subshell to ensure stderr is captured
    auto result = tool.execute(R"({"command":"bash -c 'echo error_msg >&2'"})");
    REQUIRE(result.success);
    REQUIRE(result.output.find("error_msg") != std::string::npos);
}

TEST_CASE("ShellTool: tool_name is shell", "[tools]") {
    ShellTool tool;
    REQUIRE(tool.tool_name() == "shell");
    REQUIRE_FALSE(tool.description().empty());
    REQUIRE(tool.parameters_json().find("command") != std::string::npos);
}

TEST_CASE("ShellTool: stdin data passed to command", "[tools]") {
    ShellTool tool;
    auto result = tool.execute(R"({"command":"cat","stdin":"hello from stdin"})");
    REQUIRE(result.success);
    REQUIRE(result.output == "hello from stdin");
}

TEST_CASE("ShellTool: command without stdin still works", "[tools]") {
    ShellTool tool;
    auto result = tool.execute(R"({"command":"echo no stdin"})");
    REQUIRE(result.success);
    REQUIRE(result.output.find("no stdin") != std::string::npos);
}

TEST_CASE("ShellTool: multiline stdin with wc -l", "[tools]") {
    ShellTool tool;
    auto result = tool.execute(R"({"command":"wc -l","stdin":"line1\nline2\nline3\n"})");
    REQUIRE(result.success);
    REQUIRE(result.output.find('3') != std::string::npos);
}

TEST_CASE("ShellTool: empty stdin does not hang", "[tools]") {
    ShellTool tool;
    auto result = tool.execute(R"({"command":"cat","stdin":""})");
    REQUIRE(result.success);
    REQUIRE(result.output.empty());
}

// ═══ Tool spec ═══════════════════════════════════════════════════

TEST_CASE("Tool::spec builds ToolSpec correctly", "[tools]") {
    FileReadTool tool;
    auto spec = tool.spec();
    REQUIRE(spec.name == "file_read");
    REQUIRE_FALSE(spec.description.empty());
    REQUIRE(spec.parameters_json.find("path") != std::string::npos);
}
