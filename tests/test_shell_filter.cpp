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
