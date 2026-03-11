#include <catch2/catch_test_macros.hpp>
#include "output_filter.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unistd.h>

using namespace ptrclaw;

// Helper: create a temp directory
static std::string make_temp_dir() {
    auto path = std::filesystem::temp_directory_path() / "ptrclaw_tee_XXXXXX";
    std::string tmpl = path.string();
    char* result = mkdtemp(tmpl.data());
    return result ? std::string(result) : "";
}

static std::string read_file(const std::string& path) {
    std::ifstream f(path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// ═══ Command classification (via filter_shell_output behavior) ═══

TEST_CASE("filter_shell_output: git diff strips context lines", "[shell_filter]") {
    std::string input =
        "diff --git a/foo.cpp b/foo.cpp\n"
        "index abc..def 100644\n"
        "--- a/foo.cpp\n"
        "+++ b/foo.cpp\n"
        "@@ -1,5 +1,5 @@\n"
        " unchanged line 1\n"
        " unchanged line 2\n"
        "-old line\n"
        "+new line\n"
        " unchanged line 3\n"
        " 2 files changed, 3 insertions(+), 1 deletion(-)\n";

    std::string result = filter_shell_output("git diff", input);
    REQUIRE(result.find("diff --git") != std::string::npos);
    REQUIRE(result.find("@@") != std::string::npos);
    REQUIRE(result.find("-old line") != std::string::npos);
    REQUIRE(result.find("+new line") != std::string::npos);
    REQUIRE(result.find("2 files changed") != std::string::npos);
    // Context lines (starting with space) should be stripped
    REQUIRE(result.find("unchanged line 1") == std::string::npos);
    REQUIRE(result.find("unchanged line 3") == std::string::npos);
}

TEST_CASE("filter_shell_output: git diff keeps headers", "[shell_filter]") {
    std::string input =
        "diff --git a/new.txt b/new.txt\n"
        "new file mode 100644\n"
        "index 0000000..abc1234\n"
        "--- /dev/null\n"
        "+++ b/new.txt\n"
        "@@ -0,0 +1,2 @@\n"
        "+line 1\n"
        "+line 2\n";

    std::string result = filter_shell_output("git diff --staged", input);
    REQUIRE(result.find("new file") != std::string::npos);
    REQUIRE(result.find("+line 1") != std::string::npos);
}

TEST_CASE("filter_shell_output: git status strips hints", "[shell_filter]") {
    std::string input =
        "On branch main\n"
        "Your branch is up to date with 'origin/main'.\n"
        "\n"
        "Changes not staged for commit:\n"
        "  (use \"git add <file>...\" to update what will be committed)\n"
        "  (use \"git restore <file>...\" to discard changes in working directory)\n"
        "\tmodified:   src/agent.cpp\n"
        "\n"
        "Untracked files:\n"
        "  (use \"git add <file>...\" to include in what will be committed)\n"
        "\tnew_file.txt\n";

    std::string result = filter_shell_output("git status", input);
    REQUIRE(result.find("On branch main") != std::string::npos);
    REQUIRE(result.find("modified:   src/agent.cpp") != std::string::npos);
    REQUIRE(result.find("new_file.txt") != std::string::npos);
    // Hints should be stripped
    REQUIRE(result.find("use \"git add") == std::string::npos);
    REQUIRE(result.find("use \"git restore") == std::string::npos);
}

TEST_CASE("filter_shell_output: test runner keeps failures and summary", "[shell_filter]") {
    std::string input =
        "running 5 tests\n"
        "test test_one ... ok\n"
        "test test_two ... ok\n"
        "test test_three ... FAILED\n"
        "  assertion failed: expected 1, got 2\n"
        "  at src/lib.rs:42\n"
        "test test_four ... ok\n"
        "test test_five ... ok\n"
        "\n"
        "failures:\n"
        "    test_three\n"
        "\n"
        "test result: FAILED. 4 passed; 1 failed\n";

    std::string result = filter_shell_output("cargo test", input);
    REQUIRE(result.find("FAILED") != std::string::npos);
    REQUIRE(result.find("assertion failed") != std::string::npos);
    REQUIRE(result.find("4 passed; 1 failed") != std::string::npos);
    // Passing tests should be filtered out
    REQUIRE(result.find("test_one ... ok") == std::string::npos);
}

TEST_CASE("filter_shell_output: test runner all passing shows summary", "[shell_filter]") {
    std::string input =
        "running 3 tests\n"
        "test test_one ... ok\n"
        "test test_two ... ok\n"
        "test test_three ... ok\n"
        "\n"
        "test result: ok. 3 passed; 0 failed\n";

    std::string result = filter_shell_output("pytest", input);
    // Should at least show something (summary or last lines)
    REQUIRE_FALSE(result.empty());
    REQUIRE(result.find("3 passed") != std::string::npos);
}

TEST_CASE("filter_shell_output: build log strips progress", "[shell_filter]") {
    std::string input =
        "[1/10] Compiling foo.cpp\n"
        "[2/10] Compiling bar.cpp\n"
        "[3/10] Compiling baz.cpp\n"
        "[4/10] Linking target myapp\n"
        "src/foo.cpp:10:5: error: use of undeclared identifier 'x'\n"
        "[5/10] Compiling qux.cpp\n"
        "ninja: build stopped: subcommand failed.\n";

    std::string result = filter_shell_output("ninja", input);
    REQUIRE(result.find("error: use of undeclared") != std::string::npos);
    REQUIRE(result.find("ninja: build stopped") != std::string::npos);
    // Progress lines should be stripped
    REQUIRE(result.find("Compiling foo.cpp") == std::string::npos);
    REQUIRE(result.find("Compiling bar.cpp") == std::string::npos);
}

TEST_CASE("filter_shell_output: build log keeps warnings", "[shell_filter]") {
    std::string input =
        "[1/5] Compiling main.cpp\n"
        "src/main.cpp:20:3: warning: unused variable 'x'\n"
        "[2/5] Linking target app\n"
        "Build succeeded.\n";

    std::string result = filter_shell_output("make", input);
    REQUIRE(result.find("warning: unused variable") != std::string::npos);
    REQUIRE(result.find("Build succeeded") != std::string::npos);
}

TEST_CASE("filter_shell_output: unknown command falls through to generic", "[shell_filter]") {
    std::string input = "some random output\nwith multiple lines\n";
    std::string result = filter_shell_output("ls -la", input);
    REQUIRE(result.find("some random output") != std::string::npos);
    REQUIRE(result.find("with multiple lines") != std::string::npos);
}

TEST_CASE("filter_shell_output: empty output passthrough", "[shell_filter]") {
    REQUIRE(filter_shell_output("git diff", "").empty());
}

TEST_CASE("filter_shell_output: strips ANSI before filtering", "[shell_filter]") {
    std::string input = "\033[31m-old line\033[0m\n\033[32m+new line\033[0m\n";
    std::string result = filter_shell_output("git diff", input);
    REQUIRE(result.find("\033[") == std::string::npos);
    REQUIRE(result.find("-old line") != std::string::npos);
}

// ═══ Tee system ═════════════════════════════════════════════════

TEST_CASE("tee_shell_output: writes file to disk", "[shell_filter]") {
    auto dir = make_temp_dir();
    REQUIRE_FALSE(dir.empty());

    std::string output = "full shell output\nline 2\nline 3\n";
    std::string path = tee_shell_output(output, dir);

    REQUIRE_FALSE(path.empty());
    REQUIRE(std::filesystem::exists(path));
    REQUIRE(read_file(path) == output);

    std::filesystem::remove_all(dir);
}

TEST_CASE("tee_shell_output: returns valid path in specified dir", "[shell_filter]") {
    auto dir = make_temp_dir();
    REQUIRE_FALSE(dir.empty());

    std::string path = tee_shell_output("test output", dir);
    REQUIRE(path.find(dir) == 0);
    REQUIRE(path.find(".log") != std::string::npos);

    std::filesystem::remove_all(dir);
}

TEST_CASE("tee_shell_output: creates tee directory", "[shell_filter]") {
    auto base = make_temp_dir();
    REQUIRE_FALSE(base.empty());
    std::string nested = base + "/sub/tee";

    std::string path = tee_shell_output("test", nested);
    REQUIRE_FALSE(path.empty());
    REQUIRE(std::filesystem::exists(nested));

    std::filesystem::remove_all(base);
}

TEST_CASE("tee_shell_output: empty output returns empty", "[shell_filter]") {
    REQUIRE(tee_shell_output("").empty());
}

TEST_CASE("tee_shell_output: multiple writes create separate files", "[shell_filter]") {
    auto dir = make_temp_dir();
    REQUIRE_FALSE(dir.empty());

    std::string path1 = tee_shell_output("output 1", dir);
    std::string path2 = tee_shell_output("output 2", dir);

    REQUIRE(path1 != path2);
    REQUIRE(read_file(path1) == "output 1");
    REQUIRE(read_file(path2) == "output 2");

    std::filesystem::remove_all(dir);
}

// ═══ JSON schema extraction ═════════════════════════════════════

TEST_CASE("extract_json_schema: simple object", "[shell_filter]") {
    std::string input = R"({"name":"John Doe","age":42,"active":true,"email":"john.doe@example.com","role":"senior-admin","department":"platform-engineering","location":"San Francisco, CA"})";
    std::string schema = extract_json_schema(input);
    REQUIRE_FALSE(schema.empty());
    REQUIRE(schema.find("name: string") != std::string::npos);
    REQUIRE(schema.find("age: int") != std::string::npos);
    REQUIRE(schema.find("active: bool") != std::string::npos);
}

TEST_CASE("extract_json_schema: nested object", "[shell_filter]") {
    std::string input = R"({"user":{"name":"John","email":"john@example.com","bio":"Software engineer with 10 years experience"},"count":5,"org":"Acme Corp"})";
    std::string schema = extract_json_schema(input);
    REQUIRE_FALSE(schema.empty());
    REQUIRE(schema.find("user: {") != std::string::npos);
    REQUIRE(schema.find("name: string") != std::string::npos);
    REQUIRE(schema.find("count: int") != std::string::npos);
}

TEST_CASE("extract_json_schema: array of objects", "[shell_filter]") {
    std::string input = R"([{"id":1,"name":"Alice"},{"id":2,"name":"Bob"},{"id":3,"name":"Charlie"}])";
    std::string schema = extract_json_schema(input);
    REQUIRE_FALSE(schema.empty());
    REQUIRE(schema.find("[{id: int, name: string}]") != std::string::npos);
}

TEST_CASE("extract_json_schema: not JSON returns empty", "[shell_filter]") {
    REQUIRE(extract_json_schema("not json at all").empty());
    REQUIRE(extract_json_schema("ls -la output").empty());
}

TEST_CASE("extract_json_schema: empty string returns empty", "[shell_filter]") {
    REQUIRE(extract_json_schema("").empty());
}

TEST_CASE("extract_json_schema: small JSON not replaced", "[shell_filter]") {
    // Schema wouldn't be shorter than the original
    std::string input = R"({"a":1})";
    std::string schema = extract_json_schema(input);
    // Schema "{a: int}" is roughly same length — should return empty
    REQUIRE(schema.empty());
}

TEST_CASE("extract_json_schema: verbose JSON gets replaced in filter", "[shell_filter]") {
    // Simulate a curl response through filter_shell_output
    std::string verbose_json = R"({"data":[{"id":1,"name":"Project Alpha","description":"A very long description that takes many tokens","status":"active","created_at":"2025-01-01T00:00:00Z"},{"id":2,"name":"Project Beta","description":"Another long description","status":"archived","created_at":"2025-06-15T12:00:00Z"}],"pagination":{"page":1,"per_page":20,"total":2}})";
    std::string result = filter_shell_output("curl -s https://api.example.com/projects", verbose_json);
    // Should be significantly shorter than the original
    REQUIRE(result.size() < verbose_json.size());
    REQUIRE(result.find("string") != std::string::npos);
}

TEST_CASE("extract_json_schema: handles null values", "[shell_filter]") {
    std::string input = R"({"name":"test","value":null,"count":0})";
    std::string schema = extract_json_schema(input);
    if (!schema.empty()) {
        REQUIRE(schema.find("null") != std::string::npos);
    }
}

// ═══ Short-circuit matching ═════════════════════════════════════

TEST_CASE("filter_shell_output: build short-circuits on clean Finished", "[shell_filter]") {
    std::string input =
        "[1/10] Compiling foo.cpp\n"
        "[2/10] Compiling bar.cpp\n"
        "[10/10] Linking target myapp\n"
        "    Finished release [optimized] target(s) in 5.23s\n";

    std::string result = filter_shell_output("cargo build", input);
    REQUIRE(result == "Build succeeded");
}

TEST_CASE("filter_shell_output: build does not short-circuit with errors", "[shell_filter]") {
    std::string input =
        "[1/3] Compiling foo.cpp\n"
        "error[E0308]: mismatched types\n"
        "    Finished dev [unoptimized] target(s) in 0.5s\n";

    std::string result = filter_shell_output("cargo build", input);
    REQUIRE(result != "Build succeeded");
    REQUIRE(result.find("error") != std::string::npos);
}

TEST_CASE("filter_shell_output: build does not short-circuit with warnings", "[shell_filter]") {
    std::string input =
        "[1/3] Compiling foo.cpp\n"
        "warning: unused variable `x`\n"
        "    Finished dev [unoptimized] target(s) in 0.5s\n";

    std::string result = filter_shell_output("cargo build", input);
    REQUIRE(result != "Build succeeded");
    REQUIRE(result.find("warning") != std::string::npos);
}

// ═══ Log deduplication ══════════════════════════════════════════

TEST_CASE("deduplicate_log_lines: collapses repeated lines", "[shell_filter]") {
    std::string input;
    for (int i = 0; i < 20; i++) {
        input += "ERROR: connection refused\n";
    }
    std::string result = deduplicate_log_lines(input);
    REQUIRE(result.find("[x20]") != std::string::npos);
    REQUIRE(result.find("connection refused") != std::string::npos);
    // Should be significantly shorter
    REQUIRE(result.size() < input.size() / 2);
}

TEST_CASE("deduplicate_log_lines: normalizes timestamps", "[shell_filter]") {
    std::string input;
    for (int i = 0; i < 15; i++) {
        input += "2025-01-01T12:00:" + std::to_string(10 + i) + " ERROR: timeout\n";
    }
    std::string result = deduplicate_log_lines(input);
    // All lines should be grouped despite different timestamps
    REQUIRE(result.find("[x15]") != std::string::npos);
}

TEST_CASE("deduplicate_log_lines: normalizes UUIDs", "[shell_filter]") {
    std::string input;
    for (int i = 0; i < 12; i++) {
        input += "Request 550e8400-e29b-41d4-a716-44665544" +
                 std::to_string(1000 + i) + " failed\n";
    }
    std::string result = deduplicate_log_lines(input);
    REQUIRE(result.find("[x12]") != std::string::npos);
}

TEST_CASE("deduplicate_log_lines: short output unchanged", "[shell_filter]") {
    std::string input = "line 1\nline 2\nline 3\n";
    REQUIRE(deduplicate_log_lines(input) == input);
}

TEST_CASE("deduplicate_log_lines: preserves distinct lines", "[shell_filter]") {
    std::string input;
    for (int i = 0; i < 15; i++) {
        input += "unique line " + std::to_string(i) + "\n";
    }
    // No deduplication should happen — all lines are different even normalized
    std::string result = deduplicate_log_lines(input);
    REQUIRE(result == input);
}

TEST_CASE("deduplicate_log_lines: empty input", "[shell_filter]") {
    REQUIRE(deduplicate_log_lines("").empty());
}

// ═══ Tee rotation ═══════════════════════════════════════════════

TEST_CASE("rotate_tee_files: removes oldest when over limit", "[shell_filter]") {
    auto dir = make_temp_dir();
    REQUIRE_FALSE(dir.empty());

    // Create 5 files
    for (int i = 0; i < 5; i++) {
        std::string path = dir + "/test_" + std::to_string(i) + ".log";
        std::ofstream f(path);
        f << "content " << i;
    }

    // Rotate to keep only 3
    rotate_tee_files(dir, 3);

    int count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.path().extension() == ".log") count++;
    }
    REQUIRE(count == 3);

    std::filesystem::remove_all(dir);
}

TEST_CASE("rotate_tee_files: truncates oversized files", "[shell_filter]") {
    auto dir = make_temp_dir();
    REQUIRE_FALSE(dir.empty());

    // Create a file larger than 100 bytes
    std::string path = dir + "/big.log";
    {
        std::ofstream f(path);
        for (int i = 0; i < 50; i++) f << "0123456789";  // 500 bytes
    }

    rotate_tee_files(dir, 20, 100);

    // File should be truncated
    auto size = std::filesystem::file_size(path);
    REQUIRE(size < 200);  // 100 bytes of content + truncation marker

    std::string content = read_file(path);
    REQUIRE(content.find("[...truncated at 100 bytes]") != std::string::npos);

    std::filesystem::remove_all(dir);
}

TEST_CASE("rotate_tee_files: no-op on empty dir", "[shell_filter]") {
    rotate_tee_files("/nonexistent/path");
    // Should not throw
}

// ═══ Smart truncation ═══════════════════════════════════════════

TEST_CASE("smart_truncate: small input unchanged", "[shell_filter]") {
    std::string input = "line 1\nline 2\nline 3\n";
    REQUIRE(smart_truncate(input, 10) == input);
}

TEST_CASE("smart_truncate: preserves structural lines", "[shell_filter]") {
    std::string input;
    input += "#include <iostream>\n";      // structural: include
    for (int i = 0; i < 50; i++) {
        input += "    x = x + " + std::to_string(i) + ";\n";
    }
    input += "class Foo {\n";              // structural: class
    for (int i = 0; i < 50; i++) {
        input += "    y = y + " + std::to_string(i) + ";\n";
    }
    input += "}\n";                        // structural: brace
    input += "def main():\n";              // structural: function
    for (int i = 0; i < 50; i++) {
        input += "    z = z + " + std::to_string(i) + ";\n";
    }

    std::string result = smart_truncate(input, 30);

    // Should preserve structural lines
    REQUIRE(result.find("#include") != std::string::npos);
    REQUIRE(result.find("class Foo") != std::string::npos);
    REQUIRE(result.find("def main") != std::string::npos);
    // Should have omission markers
    REQUIRE(result.find("lines omitted") != std::string::npos);
    // Should be shorter
    auto result_lines = 1;
    for (char c : result) if (c == '\n') result_lines++;
    REQUIRE(result_lines <= 35);  // allow some slack for markers
}

TEST_CASE("smart_truncate: preserves error lines", "[shell_filter]") {
    std::string input;
    for (int i = 0; i < 100; i++) {
        input += "normal line " + std::to_string(i) + "\n";
    }
    input += "src/foo.cpp:10: error: undefined reference\n";
    for (int i = 0; i < 100; i++) {
        input += "more normal " + std::to_string(i) + "\n";
    }

    std::string result = smart_truncate(input, 30);
    REQUIRE(result.find("error: undefined reference") != std::string::npos);
}

// ═══ Diagnostic grouping ════════════════════════════════════════

TEST_CASE("group_diagnostics: groups repeated warnings", "[shell_filter]") {
    std::string input =
        "src/a.cpp:10:5: warning: unused variable 'x'\n"
        "src/b.cpp:20:3: warning: unused variable 'x'\n"
        "src/c.cpp:30:1: warning: unused variable 'x'\n"
        "src/d.cpp:40:7: warning: unused variable 'x'\n"
        "src/e.cpp:50:2: warning: unused variable 'x'\n"
        "Build succeeded.\n";

    std::string result = group_diagnostics(input);
    // First occurrence kept
    REQUIRE(result.find("src/a.cpp:10:5: warning: unused variable 'x'") != std::string::npos);
    // Others grouped
    REQUIRE(result.find("4 more") != std::string::npos);
    // Duplicates removed
    REQUIRE(result.find("src/b.cpp:20:3") == std::string::npos);
    // Non-diagnostic kept
    REQUIRE(result.find("Build succeeded") != std::string::npos);
}

TEST_CASE("group_diagnostics: groups repeated errors", "[shell_filter]") {
    std::string input =
        "src/foo.cpp:1:1: error: undeclared identifier 'x'\n"
        "src/bar.cpp:2:1: error: undeclared identifier 'x'\n"
        "src/baz.cpp:3:1: error: undeclared identifier 'x'\n"
        "src/foo.cpp:5:1: warning: unused import\n"
        "src/bar.cpp:6:1: warning: unused import\n"
        "3 errors generated.\n";

    std::string result = group_diagnostics(input);
    REQUIRE(result.find("2 more") != std::string::npos);  // errors
    REQUIRE(result.find("1 more") != std::string::npos);  // warnings
    REQUIRE(result.find("3 errors generated") != std::string::npos);
}

TEST_CASE("group_diagnostics: unique diagnostics unchanged", "[shell_filter]") {
    std::string input =
        "src/a.cpp:1: error: type mismatch\n"
        "src/b.cpp:2: warning: shadow variable\n"
        "src/c.cpp:3: error: missing semicolon\n";

    REQUIRE(group_diagnostics(input) == input);
}

TEST_CASE("group_diagnostics: short input unchanged", "[shell_filter]") {
    std::string input = "one error\ntwo lines\n";
    REQUIRE(group_diagnostics(input) == input);
}

TEST_CASE("group_diagnostics: shows affected files", "[shell_filter]") {
    std::string input =
        "src/alpha.cpp:10: warning: conversion\n"
        "src/beta.cpp:20: warning: conversion\n"
        "src/gamma.cpp:30: warning: conversion\n"
        "src/delta.cpp:40: warning: conversion\n"
        "src/epsilon.cpp:50: warning: conversion\n"
        "done\n";

    std::string result = group_diagnostics(input);
    REQUIRE(result.find("src/beta.cpp") != std::string::npos);
    REQUIRE(result.find("src/gamma.cpp") != std::string::npos);
}

TEST_CASE("group_diagnostics: integrated with build filter", "[shell_filter]") {
    std::string input =
        "[1/10] Compiling a.cpp\n"
        "src/a.cpp:5: warning: unused 'x'\n"
        "[2/10] Compiling b.cpp\n"
        "src/b.cpp:5: warning: unused 'x'\n"
        "[3/10] Compiling c.cpp\n"
        "src/c.cpp:5: warning: unused 'x'\n"
        "[4/10] Linking app\n"
        "Build succeeded.\n";

    std::string result = filter_shell_output("ninja", input);
    // Warnings should be grouped
    REQUIRE(result.find("2 more") != std::string::npos);
    REQUIRE(result.find("Build succeeded") != std::string::npos);
}

// ═══ Noise directory filtering ══════════════════════════════════

TEST_CASE("filter_noise_dirs: strips node_modules", "[shell_filter]") {
    std::string input =
        ".\n"
        "├── src\n"
        "│   ├── main.cpp\n"
        "│   └── util.cpp\n"
        "├── node_modules\n"
        "│   ├── express\n"
        "│   └── lodash\n"
        "├── .git\n"
        "│   ├── objects\n"
        "│   └── refs\n"
        "└── README.md\n";

    std::string result = filter_noise_dirs(input);
    REQUIRE(result.find("src") != std::string::npos);
    REQUIRE(result.find("main.cpp") != std::string::npos);
    REQUIRE(result.find("README.md") != std::string::npos);
    REQUIRE(result.find("node_modules") == std::string::npos);
    REQUIRE(result.find(".git") == std::string::npos);
    REQUIRE(result.find("noise entries stripped") != std::string::npos);
}

TEST_CASE("filter_noise_dirs: strips from find output", "[shell_filter]") {
    std::string input =
        "./src/main.cpp\n"
        "./src/util.cpp\n"
        "./node_modules/express/index.js\n"
        "./node_modules/lodash/lodash.js\n"
        "./__pycache__/foo.pyc\n"
        "./.venv/lib/python3.11/site.py\n"
        "./README.md\n";

    std::string result = filter_noise_dirs(input);
    REQUIRE(result.find("src/main.cpp") != std::string::npos);
    REQUIRE(result.find("README.md") != std::string::npos);
    REQUIRE(result.find("node_modules") == std::string::npos);
    REQUIRE(result.find("__pycache__") == std::string::npos);
    REQUIRE(result.find(".venv") == std::string::npos);
}

TEST_CASE("filter_noise_dirs: integrated with filter_shell_output", "[shell_filter]") {
    std::string input =
        ".\n"
        "├── src\n"
        "├── node_modules\n"
        "├── .git\n"
        "├── target\n"
        "└── Makefile\n";

    std::string result = filter_shell_output("tree", input);
    REQUIRE(result.find("src") != std::string::npos);
    REQUIRE(result.find("Makefile") != std::string::npos);
    REQUIRE(result.find("node_modules") == std::string::npos);
    REQUIRE(result.find("noise entries stripped") != std::string::npos);
}

TEST_CASE("filter_noise_dirs: no noise unchanged", "[shell_filter]") {
    std::string input = "src/main.cpp\nsrc/util.cpp\nMakefile\n";
    REQUIRE(filter_noise_dirs(input) == input);
}

TEST_CASE("filter_noise_dirs: strips .DS_Store", "[shell_filter]") {
    std::string input =
        "src/main.cpp\n"
        ".DS_Store\n"
        "src/.DS_Store\n"
        "README.md\n";

    std::string result = filter_noise_dirs(input);
    REQUIRE(result.find("main.cpp") != std::string::npos);
    REQUIRE(result.find(".DS_Store") == std::string::npos);
}

// ═══ Linter output filtering ════════════════════════════════════

TEST_CASE("filter_shell_output: eslint groups repeated rules", "[shell_filter]") {
    std::string input =
        "/src/a.js:10:5: warning: Unexpected var, use let or const instead no-var\n"
        "/src/b.js:20:3: warning: Unexpected var, use let or const instead no-var\n"
        "/src/c.js:30:1: warning: Unexpected var, use let or const instead no-var\n"
        "/src/d.js:40:7: warning: Unexpected var, use let or const instead no-var\n"
        "/src/e.js:50:2: warning: Unexpected var, use let or const instead no-var\n"
        "\n"
        "5 problems (0 errors, 5 warnings)\n";

    std::string result = filter_shell_output("eslint src/", input);
    // First occurrence kept, others grouped
    REQUIRE(result.find("Unexpected var") != std::string::npos);
    REQUIRE(result.find("4 more") != std::string::npos);
    // "0 problems" lines stripped
    REQUIRE(result.find("5 problems") != std::string::npos);
}

TEST_CASE("filter_shell_output: tsc errors grouped", "[shell_filter]") {
    std::string input =
        "src/a.ts:1:1: error: Property 'x' does not exist on type 'Y'\n"
        "src/b.ts:2:1: error: Property 'x' does not exist on type 'Y'\n"
        "src/c.ts:3:1: error: Property 'x' does not exist on type 'Y'\n"
        "src/d.ts:4:1: error: Property 'x' does not exist on type 'Y'\n"
        "src/e.ts:5:1: error: Property 'x' does not exist on type 'Y'\n"
        "Found 5 errors.\n";

    std::string result = filter_shell_output("tsc --noEmit", input);
    REQUIRE(result.find("4 more") != std::string::npos);
    REQUIRE(result.find("Found 5 errors") != std::string::npos);
}

TEST_CASE("filter_shell_output: ruff classified as linter", "[shell_filter]") {
    std::string input =
        "src/a.py:10:1: F401 os imported but unused\n"
        "src/b.py:20:1: F401 os imported but unused\n"
        "src/c.py:30:1: F401 os imported but unused\n"
        "Found 3 errors.\n";

    std::string result = filter_shell_output("ruff check .", input);
    REQUIRE(result.find("F401") != std::string::npos);
}

TEST_CASE("filter_shell_output: small linter output unchanged", "[shell_filter]") {
    std::string input =
        "src/a.py:10:1: F401 os imported but unused\n"
        "Found 1 error.\n";

    std::string result = filter_shell_output("ruff check .", input);
    REQUIRE(result.find("F401") != std::string::npos);
    REQUIRE(result.find("Found 1 error") != std::string::npos);
}

// ═══ Search result filtering ════════════════════════════════════

TEST_CASE("filter_shell_output: grep groups by file", "[shell_filter]") {
    std::string input;
    for (int i = 0; i < 20; i++) {
        input += "src/big_file.cpp:" + std::to_string(i + 1) + ":    match " + std::to_string(i) + "\n";
    }
    input += "src/small_file.cpp:5:    match here\n";

    std::string result = filter_shell_output("grep -rn match src/", input);
    // Should cap matches per file
    REQUIRE(result.find("more matches in src/big_file.cpp") != std::string::npos);
    // Small file fully shown
    REQUIRE(result.find("src/small_file.cpp:5") != std::string::npos);
    // Summary at end
    REQUIRE(result.find("21 matches in 2 files") != std::string::npos);
}

TEST_CASE("filter_shell_output: rg classified as search", "[shell_filter]") {
    std::string input;
    for (int i = 0; i < 8; i++) {
        input += "src/a.cpp:" + std::to_string(i * 10 + 1) + ":    foo match " + std::to_string(i) + "\n";
    }
    input += "src/b.cpp:5:    foo qux\n";
    input += "src/c.cpp:10:    foo bar\n";
    input += "src/d.cpp:15:    foo baz\n";

    std::string result = filter_shell_output("rg foo src/", input);
    REQUIRE(result.find("11 matches in 4 files") != std::string::npos);
    // Should cap matches for src/a.cpp
    REQUIRE(result.find("more matches in src/a.cpp") != std::string::npos);
}

TEST_CASE("filter_shell_output: small search unchanged", "[shell_filter]") {
    std::string input =
        "src/a.cpp:10:    foo\n"
        "src/b.cpp:20:    foo\n";

    std::string result = filter_shell_output("grep -rn foo", input);
    REQUIRE(result.find("src/a.cpp:10") != std::string::npos);
    REQUIRE(result.find("src/b.cpp:20") != std::string::npos);
}

// ═══ HTTP response filtering ════════════════════════════════════

TEST_CASE("filter_shell_output: curl -v strips headers", "[shell_filter]") {
    std::string input =
        "* Trying 93.184.216.34...\n"
        "* Connected to example.com\n"
        "> GET / HTTP/1.1\n"
        "> Host: example.com\n"
        "> Accept: */*\n"
        ">\n"
        "< HTTP/1.1 200 OK\n"
        "< Content-Type: text/html; charset=UTF-8\n"
        "< Content-Length: 1256\n"
        "< Date: Mon, 10 Mar 2025 12:00:00 GMT\n"
        "< Server: nginx\n"
        "< X-Custom: value\n"
        "< X-Another: value2\n"
        "<\n"
        "<html><body>Hello World</body></html>\n";

    std::string result = filter_shell_output("curl -v https://example.com", input);
    // Request headers stripped
    REQUIRE(result.find("> GET") == std::string::npos);
    REQUIRE(result.find("* Trying") == std::string::npos);
    // Status and content-type kept
    REQUIRE(result.find("HTTP/1.1 200 OK") != std::string::npos);
    REQUIRE(result.find("Content-Type") != std::string::npos);
    // Other response headers stripped
    REQUIRE(result.find("X-Custom") == std::string::npos);
    // Body kept
    REQUIRE(result.find("Hello World") != std::string::npos);
    // Header count shown
    REQUIRE(result.find("headers stripped") != std::string::npos);
}

TEST_CASE("filter_shell_output: curl JSON response gets schema", "[shell_filter]") {
    std::string input =
        R"({"data":[{"id":1,"name":"Alice","email":"alice@example.com","bio":"Software engineer specializing in distributed systems"},{"id":2,"name":"Bob","email":"bob@example.com","bio":"Product manager with expertise in growth"}],"total":2,"page":1})";

    std::string result = filter_shell_output("curl -s https://api.example.com/users", input);
    // Should extract schema
    REQUIRE(result.find("string") != std::string::npos);
    REQUIRE(result.size() < input.size());
}

TEST_CASE("filter_shell_output: curl -i strips raw headers", "[shell_filter]") {
    std::string input =
        "HTTP/1.1 200 OK\n"
        "Content-Type: application/json\n"
        "X-Request-Id: abc123\n"
        "X-RateLimit-Remaining: 99\n"
        "Server: cloudflare\n"
        "\n"
        R"({"status":"ok","message":"This is a somewhat long response body that has enough content to be worth keeping in the output"})";

    std::string result = filter_shell_output("curl -i https://api.example.com", input);
    REQUIRE(result.find("HTTP/1.1 200 OK") != std::string::npos);
    REQUIRE(result.find("Content-Type") != std::string::npos);
    REQUIRE(result.find("X-Request-Id") == std::string::npos);
    REQUIRE(result.find("status") != std::string::npos);
}

// ═══ Container output filtering ═════════════════════════════════

TEST_CASE("filter_shell_output: kubectl strips metadata", "[shell_filter]") {
    std::string input =
        "apiVersion: v1\n"
        "kind: Pod\n"
        "metadata:\n"
        "    name: my-pod\n"
        "    namespace: default\n"
        "    creationTimestamp: \"2025-01-01T00:00:00Z\"\n"
        "    labels:\n"
        "        app: web\n"
        "        tier: frontend\n"
        "    annotations:\n"
        "        kubectl.kubernetes.io/last-applied: very-long-json\n"
        "    resourceVersion: \"12345\"\n"
        "    uid: 550e8400-e29b-41d4-a716-446655440000\n"
        "spec:\n"
        "    containers:\n"
        "        - name: web\n";

    std::string result = filter_shell_output("kubectl get pod my-pod -o yaml", input);
    // name and namespace kept
    REQUIRE(result.find("name: my-pod") != std::string::npos);
    REQUIRE(result.find("namespace: default") != std::string::npos);
    // Verbose metadata stripped
    REQUIRE(result.find("uid:") == std::string::npos);
    REQUIRE(result.find("resourceVersion") == std::string::npos);
    REQUIRE(result.find("metadata fields stripped") != std::string::npos);
    // Non-metadata preserved
    REQUIRE(result.find("spec:") != std::string::npos);
}

TEST_CASE("filter_shell_output: docker ps truncates long lines", "[shell_filter]") {
    std::string header = "CONTAINER ID   IMAGE   COMMAND   CREATED   STATUS   PORTS   NAMES\n";
    std::string long_line = "abc123 ";
    for (int i = 0; i < 50; i++) long_line += "very-long-command-string ";
    long_line += "\n";
    std::string normal = "def456   nginx   \"nginx\"   5m ago   Up   80/tcp   web\n";

    std::string result = filter_shell_output("docker ps", header + long_line + normal);
    REQUIRE(result.find("CONTAINER ID") != std::string::npos);
    REQUIRE(result.find("def456") != std::string::npos);
    // Long line truncated
    REQUIRE(result.find("...") != std::string::npos);
}

TEST_CASE("filter_shell_output: small docker output unchanged", "[shell_filter]") {
    std::string input =
        "CONTAINER ID   IMAGE\n"
        "abc123   nginx\n";

    std::string result = filter_shell_output("docker ps", input);
    REQUIRE(result.find("abc123") != std::string::npos);
}

// ═══ Package manager filtering ══════════════════════════════════

TEST_CASE("filter_shell_output: npm list hides deep deps", "[shell_filter]") {
    std::string input;
    input += "my-project@1.0.0\n";
    input += "├── express@4.18.0\n";
    input += "│   ├── accepts@1.3.8\n";
    input += "│   │   ├── mime-types@2.1.35\n";
    input += "│   │   │   └── mime-db@1.52.0\n";
    input += "│   │   └── negotiator@0.6.3\n";
    input += "│   ├── body-parser@1.20.2\n";
    input += "│   │   ├── bytes@3.1.2\n";
    input += "│   │   └── depd@2.0.0\n";
    input += "├── lodash@4.17.21\n";
    input += "└── typescript@5.0.0\n";

    std::string result = filter_shell_output("npm list", input);
    // Top-level deps kept
    REQUIRE(result.find("express@4.18.0") != std::string::npos);
    REQUIRE(result.find("lodash@4.17.21") != std::string::npos);
    // Deep transitive deps hidden
    REQUIRE(result.find("transitive dependencies hidden") != std::string::npos);
}

TEST_CASE("filter_shell_output: cargo tree hides deep deps", "[shell_filter]") {
    std::string input;
    input += "my-crate v0.1.0\n";
    input += "├── serde v1.0\n";
    input += "│   ├── serde_derive v1.0\n";
    input += "│   │   ├── proc-macro2 v1.0\n";
    input += "│   │   │   └── unicode-ident v1.0\n";
    input += "│   │   ├── quote v1.0\n";
    input += "│   │   │   └── proc-macro2 v1.0\n";
    input += "│   │   └── syn v2.0\n";
    input += "│   │       └── proc-macro2 v1.0\n";
    input += "├── tokio v1.0\n";
    input += "└── anyhow v1.0\n";

    std::string result = filter_shell_output("cargo tree", input);
    REQUIRE(result.find("serde v1.0") != std::string::npos);
    REQUIRE(result.find("tokio v1.0") != std::string::npos);
    REQUIRE(result.find("transitive dependencies hidden") != std::string::npos);
}

TEST_CASE("filter_shell_output: pip list small unchanged", "[shell_filter]") {
    std::string input =
        "Package    Version\n"
        "---------- -------\n"
        "pip        23.0\n"
        "setuptools 67.0\n";

    std::string result = filter_shell_output("pip list", input);
    REQUIRE(result.find("pip") != std::string::npos);
    REQUIRE(result.find("setuptools") != std::string::npos);
}

TEST_CASE("filter_shell_output: pip freeze classified as package", "[shell_filter]") {
    std::string input;
    for (int i = 0; i < 15; i++) {
        input += "package-" + std::to_string(i) + "==1.0.0\n";
    }

    std::string result = filter_shell_output("pip freeze", input);
    // All lines are top-level (no indentation), so all kept
    REQUIRE(result.find("package-0==1.0.0") != std::string::npos);
    REQUIRE(result.find("package-14==1.0.0") != std::string::npos);
}
