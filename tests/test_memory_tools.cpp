#include <catch2/catch_test_macros.hpp>
#include "memory/json_memory.hpp"
#include "tools/memory_store.hpp"
#include "tools/memory_recall.hpp"
#include "tools/memory_forget.hpp"
#include "tools/memory_link.hpp"
#include <filesystem>
#include <unistd.h>

using namespace ptrclaw;

static std::string tool_test_path() {
    return "/tmp/ptrclaw_test_tools_" + std::to_string(getpid()) + ".json";
}

struct ToolTestFixture {
    std::string path = tool_test_path();
    JsonMemory mem{path};
    MemoryStoreTool store_tool;
    MemoryRecallTool recall_tool;
    MemoryForgetTool forget_tool;

    ToolTestFixture() {
        store_tool.set_memory(&mem);
        recall_tool.set_memory(&mem);
        forget_tool.set_memory(&mem);
    }

    ~ToolTestFixture() {
        std::filesystem::remove(path);
    }
};

// ── memory_store ─────────────────────────────────────────────

TEST_CASE("MemoryStoreTool: stores a memory entry", "[memory_tools]") {
    ToolTestFixture f;

    auto result = f.store_tool.execute(
        R"({"key":"lang","content":"Python","category":"knowledge"})");
    REQUIRE(result.success);
    REQUIRE(result.output.find("lang") != std::string::npos);

    auto entry = f.mem.get("lang");
    REQUIRE(entry.has_value());
    REQUIRE(entry.value_or(MemoryEntry{}).content == "Python");
}

TEST_CASE("MemoryStoreTool: fails without memory", "[memory_tools]") {
    MemoryStoreTool tool;
    auto result = tool.execute(R"({"key":"x","content":"y"})");
    REQUIRE_FALSE(result.success);
    REQUIRE(result.output.find("not enabled") != std::string::npos);
}

TEST_CASE("MemoryStoreTool: fails on missing key", "[memory_tools]") {
    ToolTestFixture f;
    auto result = f.store_tool.execute(R"({"content":"hello"})");
    REQUIRE_FALSE(result.success);
}

TEST_CASE("MemoryStoreTool: fails on missing content", "[memory_tools]") {
    ToolTestFixture f;
    auto result = f.store_tool.execute(R"({"key":"x"})");
    REQUIRE_FALSE(result.success);
}

TEST_CASE("MemoryStoreTool: fails on bad JSON", "[memory_tools]") {
    ToolTestFixture f;
    auto result = f.store_tool.execute("not json");
    REQUIRE_FALSE(result.success);
}

// ── memory_recall ────────────────────────────────────────────

TEST_CASE("MemoryRecallTool: recalls stored memories", "[memory_tools]") {
    ToolTestFixture f;

    f.mem.store("language", "Python is preferred", MemoryCategory::Knowledge, "");

    auto result = f.recall_tool.execute(R"({"query":"language"})");
    REQUIRE(result.success);
    REQUIRE(result.output.find("Python") != std::string::npos);
}

TEST_CASE("MemoryRecallTool: returns message when nothing found", "[memory_tools]") {
    ToolTestFixture f;

    auto result = f.recall_tool.execute(R"({"query":"nonexistent"})");
    REQUIRE(result.success);
    REQUIRE(result.output.find("No matching") != std::string::npos);
}

TEST_CASE("MemoryRecallTool: fails without memory", "[memory_tools]") {
    MemoryRecallTool tool;
    auto result = tool.execute(R"({"query":"test"})");
    REQUIRE_FALSE(result.success);
}

TEST_CASE("MemoryRecallTool: fails on missing query", "[memory_tools]") {
    ToolTestFixture f;
    auto result = f.recall_tool.execute(R"({})");
    REQUIRE_FALSE(result.success);
}

// ── memory_forget ────────────────────────────────────────────

TEST_CASE("MemoryForgetTool: forgets a stored memory", "[memory_tools]") {
    ToolTestFixture f;

    f.mem.store("temp", "delete me", MemoryCategory::Conversation, "");

    auto result = f.forget_tool.execute(R"({"key":"temp"})");
    REQUIRE(result.success);
    REQUIRE(result.output.find("Forgot") != std::string::npos);
    REQUIRE_FALSE(f.mem.get("temp").has_value());
}

TEST_CASE("MemoryForgetTool: returns error for missing key", "[memory_tools]") {
    ToolTestFixture f;

    auto result = f.forget_tool.execute(R"({"key":"ghost"})");
    REQUIRE_FALSE(result.success);
    REQUIRE(result.output.find("No memory found") != std::string::npos);
}

TEST_CASE("MemoryForgetTool: fails without memory", "[memory_tools]") {
    MemoryForgetTool tool;
    auto result = tool.execute(R"({"key":"x"})");
    REQUIRE_FALSE(result.success);
}

// ── memory_link ──────────────────────────────────────────────

TEST_CASE("MemoryLinkTool: links two entries", "[memory_tools]") {
    ToolTestFixture f;
    MemoryLinkTool link_tool;
    link_tool.set_memory(&f.mem);

    f.mem.store("entry-a", "About A", MemoryCategory::Knowledge, "");
    f.mem.store("entry-b", "About B", MemoryCategory::Knowledge, "");

    auto result = link_tool.execute(R"({"action":"link","from":"entry-a","to":"entry-b"})");
    REQUIRE(result.success);
    REQUIRE(result.output.find("Linked") != std::string::npos);

    auto a = f.mem.get("entry-a");
    REQUIRE(a.value_or(MemoryEntry{}).links.size() == 1);
}

TEST_CASE("MemoryLinkTool: unlinks two entries", "[memory_tools]") {
    ToolTestFixture f;
    MemoryLinkTool link_tool;
    link_tool.set_memory(&f.mem);

    f.mem.store("entry-a", "About A", MemoryCategory::Knowledge, "");
    f.mem.store("entry-b", "About B", MemoryCategory::Knowledge, "");
    f.mem.link("entry-a", "entry-b");

    auto result = link_tool.execute(R"({"action":"unlink","from":"entry-a","to":"entry-b"})");
    REQUIRE(result.success);
    REQUIRE(result.output.find("Unlinked") != std::string::npos);
}

TEST_CASE("MemoryLinkTool: fails without memory", "[memory_tools]") {
    MemoryLinkTool link_tool;
    auto result = link_tool.execute(R"({"action":"link","from":"a","to":"b"})");
    REQUIRE_FALSE(result.success);
    REQUIRE(result.output.find("not enabled") != std::string::npos);
}

TEST_CASE("MemoryLinkTool: fails for missing entry", "[memory_tools]") {
    ToolTestFixture f;
    MemoryLinkTool link_tool;
    link_tool.set_memory(&f.mem);

    f.mem.store("exists", "content", MemoryCategory::Knowledge, "");

    auto result = link_tool.execute(R"({"action":"link","from":"exists","to":"ghost"})");
    REQUIRE_FALSE(result.success);
}

// ── memory_store with links ─────────────────────────────────

TEST_CASE("MemoryStoreTool: stores with links", "[memory_tools]") {
    ToolTestFixture f;

    f.mem.store("target", "target content", MemoryCategory::Knowledge, "");

    auto result = f.store_tool.execute(
        R"({"key":"source","content":"source content","links":["target"]})");
    REQUIRE(result.success);

    auto source = f.mem.get("source");
    REQUIRE(source.has_value());
    REQUIRE(source.value_or(MemoryEntry{}).links.size() == 1);
    REQUIRE(source.value_or(MemoryEntry{}).links[0] == "target");
}

// ── memory_recall with depth ────────────────────────────────

TEST_CASE("MemoryRecallTool: recall with depth follows links", "[memory_tools]") {
    ToolTestFixture f;

    f.mem.store("alpha-concept", "Alpha is about algorithms", MemoryCategory::Knowledge, "");
    f.mem.store("beta-detail", "Beta provides implementation specifics", MemoryCategory::Knowledge, "");
    f.mem.link("alpha-concept", "beta-detail");

    auto result = f.recall_tool.execute(R"({"query":"alpha algorithms","depth":1})");
    REQUIRE(result.success);
    REQUIRE(result.output.find("alpha-concept") != std::string::npos);
    // With depth=1, the linked entry should appear as "(linked)"
    REQUIRE(result.output.find("beta-detail") != std::string::npos);
    REQUIRE(result.output.find("linked") != std::string::npos);
}

// ── Tool metadata ────────────────────────────────────────────

TEST_CASE("Memory tools: names and descriptions", "[memory_tools]") {
    MemoryStoreTool store;
    MemoryRecallTool recall;
    MemoryForgetTool forget;
    MemoryLinkTool link;

    REQUIRE(store.tool_name() == "memory_store");
    REQUIRE(recall.tool_name() == "memory_recall");
    REQUIRE(forget.tool_name() == "memory_forget");
    REQUIRE(link.tool_name() == "memory_link");

    REQUIRE_FALSE(store.description().empty());
    REQUIRE_FALSE(recall.description().empty());
    REQUIRE_FALSE(forget.description().empty());
    REQUIRE_FALSE(link.description().empty());

    REQUIRE_FALSE(store.parameters_json().empty());
    REQUIRE_FALSE(recall.parameters_json().empty());
    REQUIRE_FALSE(forget.parameters_json().empty());
    REQUIRE_FALSE(link.parameters_json().empty());
}
