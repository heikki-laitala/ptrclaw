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
    std::string simple_response = "simple response";

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
        return simple_response;
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

TEST_CASE("build_hatch_prompt: covers all three sections", "[hatch]") {
    auto prompt = build_hatch_prompt();
    REQUIRE(prompt.find("soul:identity") != std::string::npos);
    REQUIRE(prompt.find("soul:user") != std::string::npos);
    REQUIRE(prompt.find("soul:philosophy") != std::string::npos);
}

// ── parse_soul_json ─────────────────────────────────────────────

TEST_CASE("parse_soul_json: extracts valid soul block", "[hatch]") {
    std::string text = R"(Here's your soul!
<soul>
[
  {"key": "soul:identity", "content": "Name: Aria\nNature: AI assistant"},
  {"key": "soul:user", "content": "Name: Henri\nTimezone: Europe/Helsinki"},
  {"key": "soul:philosophy", "content": "Core truths: Be genuine"}
]
</soul>
Done!)";

    auto result = parse_soul_json(text);
    REQUIRE(result.found());
    REQUIRE(result.entries.size() == 3);
    REQUIRE(result.entries[0].first == "soul:identity");
    REQUIRE(result.entries[1].first == "soul:user");
    REQUIRE(result.entries[2].first == "soul:philosophy");
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

TEST_CASE("build_soul_block: formats three-section soul correctly", "[hatch]") {
    std::string path = temp_memory_path("soul_block");
    auto mem = std::make_unique<JsonMemory>(path);

    mem->store("soul:identity", "Name: Aria\nNature: AI assistant\nVibe: Warm", MemoryCategory::Core, "");
    mem->store("soul:user", "Name: Henri\nTimezone: Europe/Helsinki", MemoryCategory::Core, "");
    mem->store("soul:philosophy", "Core truths: Be genuine\nBoundaries: None", MemoryCategory::Core, "");
    mem->store("other-core", "Not a soul entry.", MemoryCategory::Core, "");

    std::string block = build_soul_block(mem.get());
    REQUIRE(block.find("Your Identity") != std::string::npos);
    REQUIRE(block.find("About you (the AI):\nName: Aria") != std::string::npos);
    REQUIRE(block.find("About your human:\nName: Henri") != std::string::npos);
    REQUIRE(block.find("Your philosophy:\nCore truths: Be genuine") != std::string::npos);
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

TEST_CASE("Agent: soul extraction stores three-section entries and exits hatching", "[hatch]") {
    auto provider = std::make_unique<HatchMockProvider>();
    auto* mock = provider.get();

    std::string soul_response =
        "Great! Here's your soul:\n"
        "<soul>\n"
        "[{\"key\":\"soul:identity\",\"content\":\"Name: Aria.\\nNature: AI assistant.\"},"
        "{\"key\":\"soul:user\",\"content\":\"Name: Henri.\\nTimezone: UTC.\"},"
        "{\"key\":\"soul:philosophy\",\"content\":\"Core truths: Be genuine.\"}]\n"
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

    // Verify all three entries stored
    auto identity = agent.memory()->get("soul:identity");
    REQUIRE(identity.has_value());
    REQUIRE(identity.value_or(MemoryEntry{}).content == "Name: Aria.\nNature: AI assistant.");

    auto user = agent.memory()->get("soul:user");
    REQUIRE(user.has_value());
    REQUIRE(user.value_or(MemoryEntry{}).content == "Name: Henri.\nTimezone: UTC.");

    auto philosophy = agent.memory()->get("soul:philosophy");
    REQUIRE(philosophy.has_value());
    REQUIRE(philosophy.value_or(MemoryEntry{}).content == "Core truths: Be genuine.");

    std::filesystem::remove(path);
}

TEST_CASE("Agent: re-hatch overwrites existing soul entries", "[hatch]") {
    auto provider = std::make_unique<HatchMockProvider>();
    auto* mock = provider.get();

    std::string soul_response =
        "<soul>\n"
        "[{\"key\":\"soul:identity\",\"content\":\"Name: Aria.\"},"
        "{\"key\":\"soul:user\",\"content\":\"Name: Henri.\"},"
        "{\"key\":\"soul:philosophy\",\"content\":\"Be genuine.\"}]\n"
        "</soul>";
    mock->next_response.content = soul_response;

    std::vector<std::unique_ptr<Tool>> tools;
    Config cfg;
    Agent agent(std::move(provider), std::move(tools), cfg);

    std::string path = temp_memory_path("soul_rehatch");
    auto mem = std::make_unique<JsonMemory>(path);
    mem->store("soul:identity", "Old identity", MemoryCategory::Core, "");
    agent.set_memory(std::move(mem));

    agent.start_hatch();
    agent.process("redo it");

    // New keys should overwrite old
    REQUIRE(agent.memory()->get("soul:identity").value_or(MemoryEntry{}).content == "Name: Aria.");
    REQUIRE(agent.memory()->get("soul:user").has_value());
    REQUIRE(agent.memory()->get("soul:philosophy").has_value());

    std::filesystem::remove(path);
}

TEST_CASE("Agent: hatching synthesizes user knowledge from conversation", "[hatch]") {
    auto provider = std::make_unique<HatchMockProvider>();
    auto* mock = provider.get();

    std::string soul_response =
        "<soul>\n"
        "[{\"key\":\"soul:identity\",\"content\":\"Name: Aria.\"},"
        "{\"key\":\"soul:user\",\"content\":\"Name: Henri.\"},"
        "{\"key\":\"soul:philosophy\",\"content\":\"Be genuine.\"}]\n"
        "</soul>";
    mock->next_response.content = soul_response;

    // Synthesis will extract this from the hatching conversation
    mock->simple_response =
        R"([{"key":"user-likes-cpp","content":"User enjoys C++ and systems programming","category":"knowledge"}])";

    std::vector<std::unique_ptr<Tool>> tools;
    Config cfg;
    cfg.memory.synthesis = true;
    cfg.memory.synthesis_interval = 1;
    Agent agent(std::move(provider), std::move(tools), cfg);

    std::string path = temp_memory_path("soul_synth");
    agent.set_memory(std::make_unique<JsonMemory>(path));
    agent.start_hatch();

    agent.process("I love C++ and systems programming");

    // Soul entries stored
    REQUIRE(agent.memory()->get("soul:identity").has_value());

    // Synthesized knowledge from user's hatching messages
    auto note = agent.memory()->get("user-likes-cpp");
    REQUIRE(note.has_value());
    REQUIRE(note.value_or(MemoryEntry{}).content == "User enjoys C++ and systems programming");

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

// ── Learned traits in soul block ────────────────────────────────

TEST_CASE("build_soul_block: renders learned traits section", "[hatch]") {
    std::string path = temp_memory_path("soul_traits");
    auto mem = std::make_unique<JsonMemory>(path);

    mem->store("soul:identity", "Name: Aria.", MemoryCategory::Core, "");
    mem->store("personality:prefers-examples", "User learns best through concrete code examples", MemoryCategory::Core, "");
    mem->store("personality:dislikes-verbosity", "User prefers concise responses", MemoryCategory::Core, "");

    std::string block = build_soul_block(mem.get());
    REQUIRE(block.find("Learned traits:") != std::string::npos);
    REQUIRE(block.find("User learns best through concrete code examples") != std::string::npos);
    REQUIRE(block.find("User prefers concise responses") != std::string::npos);

    std::filesystem::remove(path);
}

TEST_CASE("build_soul_block: caps learned traits at 5", "[hatch]") {
    std::string path = temp_memory_path("soul_traits_cap");
    auto mem = std::make_unique<JsonMemory>(path);

    mem->store("soul:identity", "Name: Aria.", MemoryCategory::Core, "");
    for (int i = 0; i < 7; i++) {
        mem->store("personality:trait-" + std::to_string(i),
                   "Trait number " + std::to_string(i), MemoryCategory::Core, "");
    }

    std::string block = build_soul_block(mem.get());
    REQUIRE(block.find("Learned traits:") != std::string::npos);

    // Count bullet points in learned traits section
    size_t pos = block.find("Learned traits:");
    size_t end = block.find("\n\n", pos);
    std::string section = block.substr(pos, end - pos);
    int count = 0;
    size_t search = 0;
    while ((search = section.find("\n- ", search)) != std::string::npos) {
        count++;
        search++;
    }
    REQUIRE(count == 5);

    std::filesystem::remove(path);
}

TEST_CASE("build_soul_block: learned traits sorted by recency", "[hatch]") {
    std::string path = temp_memory_path("soul_traits_order");
    auto mem = std::make_unique<JsonMemory>(path);

    mem->store("soul:identity", "Name: Aria.", MemoryCategory::Core, "");
    // Use snapshot_import to set explicit timestamps for deterministic ordering
    mem->snapshot_import(R"([
        {"key":"personality:old","content":"Old trait","category":"core","timestamp":1000},
        {"key":"personality:mid","content":"Mid trait","category":"core","timestamp":2000},
        {"key":"personality:new","content":"New trait","category":"core","timestamp":3000}
    ])");

    std::string block = build_soul_block(mem.get());
    // Most recent (highest timestamp) should appear first
    size_t pos_new = block.find("New trait");
    size_t pos_mid = block.find("Mid trait");
    size_t pos_old = block.find("Old trait");
    REQUIRE(pos_new != std::string::npos);
    REQUIRE(pos_mid != std::string::npos);
    REQUIRE(pos_old != std::string::npos);
    REQUIRE(pos_new < pos_mid);
    REQUIRE(pos_mid < pos_old);

    std::filesystem::remove(path);
}

TEST_CASE("build_soul_block: learned traits coexist with three soul sections", "[hatch]") {
    std::string path = temp_memory_path("soul_traits_combined");
    auto mem = std::make_unique<JsonMemory>(path);

    mem->store("soul:identity", "Name: Aria.", MemoryCategory::Core, "");
    mem->store("soul:user", "Name: Henri.", MemoryCategory::Core, "");
    mem->store("soul:philosophy", "Be genuine.", MemoryCategory::Core, "");
    mem->store("personality:likes-humor", "User responds well to light humor", MemoryCategory::Core, "");

    std::string block = build_soul_block(mem.get());
    REQUIRE(block.find("Your Identity") != std::string::npos);
    REQUIRE(block.find("About you (the AI):") != std::string::npos);
    REQUIRE(block.find("About your human:") != std::string::npos);
    REQUIRE(block.find("Your philosophy:") != std::string::npos);
    REQUIRE(block.find("Learned traits:") != std::string::npos);
    REQUIRE(block.find("User responds well to light humor") != std::string::npos);
    REQUIRE(block.find("Embody this persona") != std::string::npos);

    std::filesystem::remove(path);
}
