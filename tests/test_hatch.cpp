#include <catch2/catch_test_macros.hpp>
#include "agent.hpp"
#include "prompt.hpp"
#include "memory/json_memory.hpp"
#include <filesystem>
#include <unistd.h>

using namespace ptrclaw;

// ── Mock provider for hatch tests ───────────────────────────────

class HatchMockProvider : public Provider {
public:
    ~HatchMockProvider() override = default;

    ChatResponse next_response;
    std::vector<ChatMessage> last_messages;
    int chat_call_count = 0;

    ChatResponse chat(const std::vector<ChatMessage>& messages,
                      const std::vector<ToolSpec>&,
                      const std::string&,
                      double) override {
        chat_call_count++;
        last_messages = messages;
        return next_response;
    }

    std::string chat_simple(const std::string&,
                            const std::string&,
                            const std::string&,
                            double) override {
        return "simple response";
    }

    bool supports_native_tools() const override { return true; }
    std::string provider_name() const override { return "hatch_mock"; }
};

// Helper to get a temp memory path
static std::string temp_memory_path(const std::string& suffix) {
    return "/tmp/ptrclaw_test_hatch_" + suffix + "_" +
           std::to_string(getpid()) + ".json";
}

// ── build_hatch_prompt ──────────────────────────────────────────

TEST_CASE("build_hatch_prompt: returns non-empty string", "[hatch]") {
    auto prompt = build_hatch_prompt();
    REQUIRE_FALSE(prompt.empty());
    REQUIRE(prompt.find("soul-hatching") != std::string::npos);
    REQUIRE(prompt.find("<soul>") != std::string::npos);
}

// ── parse_soul_json ─────────────────────────────────────────────

TEST_CASE("parse_soul_json: extracts valid soul block", "[hatch]") {
    std::string text = R"(Here's your soul!
<soul>
[
  {"key": "soul:identity", "content": "Name: Aria. Nature: AI assistant."},
  {"key": "soul:vibe", "content": "Warm and concise."}
]
</soul>
Done!)";

    auto result = parse_soul_json(text);
    REQUIRE(result.found());
    REQUIRE(result.entries.size() == 2);
    REQUIRE(result.entries[0].first == "soul:identity");
    REQUIRE(result.entries[0].second == "Name: Aria. Nature: AI assistant.");
    REQUIRE(result.entries[1].first == "soul:vibe");
    REQUIRE(result.entries[1].second == "Warm and concise.");
    REQUIRE(result.block_start == text.find("<soul>"));
    REQUIRE(result.block_end == text.find("</soul>") + 7);
}

TEST_CASE("parse_soul_json: returns empty on no tags", "[hatch]") {
    auto result = parse_soul_json("Just a regular response.");
    REQUIRE_FALSE(result.found());
}

TEST_CASE("parse_soul_json: returns empty on malformed JSON", "[hatch]") {
    std::string text = "<soul>not json</soul>";
    auto result = parse_soul_json(text);
    REQUIRE_FALSE(result.found());
}

TEST_CASE("parse_soul_json: skips entries without key or content", "[hatch]") {
    std::string text = R"(<soul>[{"key":"soul:x"},{"content":"y"},{"key":"soul:z","content":"ok"}]</soul>)";
    auto result = parse_soul_json(text);
    REQUIRE(result.found());
    REQUIRE(result.entries.size() == 1);
    REQUIRE(result.entries[0].first == "soul:z");
}

TEST_CASE("parse_soul_json: returns empty on mismatched tags", "[hatch]") {
    std::string text = "<soul>[{\"key\":\"a\",\"content\":\"b\"}]";
    auto result = parse_soul_json(text);
    REQUIRE_FALSE(result.found());
}

// ── build_soul_block ────────────────────────────────────────────

TEST_CASE("build_soul_block: returns empty for null memory", "[hatch]") {
    REQUIRE(build_soul_block(nullptr).empty());
}

TEST_CASE("build_soul_block: returns empty when no soul entries exist", "[hatch]") {
    std::string path = temp_memory_path("soul_empty");
    auto mem = std::make_unique<JsonMemory>(path);

    mem->store("some-key", "some value", MemoryCategory::Knowledge, "");
    REQUIRE(build_soul_block(mem.get()).empty());

    std::filesystem::remove(path);
}

TEST_CASE("build_soul_block: formats soul entries correctly", "[hatch]") {
    std::string path = temp_memory_path("soul_block");
    auto mem = std::make_unique<JsonMemory>(path);

    mem->store("soul:identity", "Name: Aria.", MemoryCategory::Core, "");
    mem->store("soul:vibe", "Direct and warm.", MemoryCategory::Core, "");
    mem->store("other-core", "Not a soul entry.", MemoryCategory::Core, "");

    std::string block = build_soul_block(mem.get());
    REQUIRE(block.find("Your Identity") != std::string::npos);
    REQUIRE(block.find("Identity: Name: Aria.") != std::string::npos);
    REQUIRE(block.find("Vibe: Direct and warm.") != std::string::npos);
    REQUIRE(block.find("Not a soul entry") == std::string::npos);
    REQUIRE(block.find("Embody this persona") != std::string::npos);

    std::filesystem::remove(path);
}

// ── is_hatched ──────────────────────────────────────────────────

TEST_CASE("Agent: is_hatched returns false on fresh memory", "[hatch]") {
    auto provider = std::make_unique<HatchMockProvider>();
    std::vector<std::unique_ptr<Tool>> tools;
    Config cfg;
    Agent agent(std::move(provider), std::move(tools), cfg);

    std::string path = temp_memory_path("is_hatched_fresh");
    agent.set_memory(std::make_unique<JsonMemory>(path));

    REQUIRE_FALSE(agent.is_hatched());

    std::filesystem::remove(path);
}

TEST_CASE("Agent: is_hatched returns true after storing soul:identity", "[hatch]") {
    auto provider = std::make_unique<HatchMockProvider>();
    std::vector<std::unique_ptr<Tool>> tools;
    Config cfg;
    Agent agent(std::move(provider), std::move(tools), cfg);

    std::string path = temp_memory_path("is_hatched_stored");
    auto mem = std::make_unique<JsonMemory>(path);
    mem->store("soul:identity", "Name: Aria.", MemoryCategory::Core, "");
    agent.set_memory(std::move(mem));

    REQUIRE(agent.is_hatched());

    std::filesystem::remove(path);
}

// ── start_hatch / hatching ──────────────────────────────────────

TEST_CASE("Agent: start_hatch enables hatching mode", "[hatch]") {
    auto provider = std::make_unique<HatchMockProvider>();
    std::vector<std::unique_ptr<Tool>> tools;
    Config cfg;
    Agent agent(std::move(provider), std::move(tools), cfg);

    REQUIRE_FALSE(agent.hatching());
    agent.start_hatch();
    REQUIRE(agent.hatching());
    REQUIRE(agent.history_size() == 0);
}

// ── Hatching uses hatch prompt ──────────────────────────────────

TEST_CASE("Agent: hatching mode uses hatch system prompt", "[hatch]") {
    auto provider = std::make_unique<HatchMockProvider>();
    auto* mock = provider.get();
    mock->next_response.content = "What name would you like?";

    std::vector<std::unique_ptr<Tool>> tools;
    Config cfg;
    Agent agent(std::move(provider), std::move(tools), cfg);

    agent.start_hatch();
    agent.process("hi");

    REQUIRE_FALSE(mock->last_messages.empty());
    REQUIRE(mock->last_messages[0].role == Role::System);
    REQUIRE(mock->last_messages[0].content.find("soul-hatching") != std::string::npos);
}

// ── Soul extraction in process ──────────────────────────────────

TEST_CASE("Agent: soul extraction stores entries and exits hatching", "[hatch]") {
    auto provider = std::make_unique<HatchMockProvider>();
    auto* mock = provider.get();

    std::string soul_response =
        "Great! Here's your soul:\n"
        "<soul>\n"
        "[{\"key\":\"soul:identity\",\"content\":\"Name: Aria.\"},"
        "{\"key\":\"soul:vibe\",\"content\":\"Warm.\"}]\n"
        "</soul>";
    mock->next_response.content = soul_response;

    std::vector<std::unique_ptr<Tool>> tools;
    Config cfg;
    Agent agent(std::move(provider), std::move(tools), cfg);

    std::string path = temp_memory_path("soul_extract");
    agent.set_memory(std::make_unique<JsonMemory>(path));
    agent.start_hatch();

    std::string reply = agent.process("done");
    REQUIRE(reply.find("Soul hatched!") != std::string::npos);
    REQUIRE(reply.find("<soul>") == std::string::npos);
    REQUIRE_FALSE(agent.hatching());
    REQUIRE(agent.is_hatched());

    // Verify entries stored
    auto identity = agent.memory()->get("soul:identity");
    REQUIRE(identity.has_value());
    REQUIRE(identity.value_or(MemoryEntry{}).content == "Name: Aria.");

    auto vibe = agent.memory()->get("soul:vibe");
    REQUIRE(vibe.has_value());
    REQUIRE(vibe.value_or(MemoryEntry{}).content == "Warm.");

    std::filesystem::remove(path);
}

TEST_CASE("Agent: hatching continues when no soul block in response", "[hatch]") {
    auto provider = std::make_unique<HatchMockProvider>();
    auto* mock = provider.get();
    mock->next_response.content = "Tell me more about your preferences.";

    std::vector<std::unique_ptr<Tool>> tools;
    Config cfg;
    Agent agent(std::move(provider), std::move(tools), cfg);

    std::string path = temp_memory_path("soul_continue");
    agent.set_memory(std::make_unique<JsonMemory>(path));
    agent.start_hatch();

    std::string reply = agent.process("I like concise responses");
    REQUIRE(reply == "Tell me more about your preferences.");
    REQUIRE(agent.hatching());

    std::filesystem::remove(path);
}

// ── build_system_prompt includes soul block ─────────────────────

TEST_CASE("build_system_prompt: includes soul block when soul entries exist", "[hatch]") {
    std::string path = temp_memory_path("sys_prompt_soul");
    auto mem = std::make_unique<JsonMemory>(path);
    mem->store("soul:identity", "Name: Aria.", MemoryCategory::Core, "");

    std::vector<std::unique_ptr<Tool>> tools;
    auto result = build_system_prompt(tools, false, false, mem.get());
    REQUIRE(result.find("Your Identity") != std::string::npos);
    REQUIRE(result.find("Name: Aria.") != std::string::npos);

    std::filesystem::remove(path);
}

TEST_CASE("build_system_prompt: no soul block when memory is null", "[hatch]") {
    std::vector<std::unique_ptr<Tool>> tools;
    auto result = build_system_prompt(tools, false, false, nullptr);
    REQUIRE(result.find("Your Identity") == std::string::npos);
}
