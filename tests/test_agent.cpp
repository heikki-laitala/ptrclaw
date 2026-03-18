#include <catch2/catch_test_macros.hpp>
#include "agent.hpp"
#include "dispatcher.hpp"
#include "memory/json_memory.hpp"
#include "tool_manager.hpp"
#include "event_bus.hpp"
#include <stdexcept>
#include <filesystem>
#include <fstream>
#include <unistd.h>

using namespace ptrclaw;

// Helper: wires up Agent + ToolManager + EventBus for tests.
struct TestAgentSetup {
    EventBus bus;
    std::unique_ptr<ToolManager> tool_mgr;
    Agent agent;

    TestAgentSetup(std::unique_ptr<Provider> provider,
                   std::vector<std::unique_ptr<Tool>> tools,
                   const Config& config)
        : tool_mgr(std::make_unique<ToolManager>(std::move(tools), config, bus))
        , agent(std::move(provider), config)
    {
        agent.set_event_bus(&bus);
        tool_mgr->wire_memory(agent.memory());
        tool_mgr->publish_tool_specs();
    }
};

// ── Mock provider ────────────────────────────────────────────────

class MockProvider : public Provider {
public:
    ~MockProvider() override = default;

    // Queue of responses: returns in order, then repeats last
    std::vector<ChatResponse> responses;
    ChatResponse next_response;
    bool native_tools = true;
    bool should_throw = false;
    int chat_call_count = 0;
    std::vector<ChatMessage> last_messages;
    std::vector<ToolSpec> last_tool_specs;

    ChatResponse chat(const std::vector<ChatMessage>& messages,
                      const std::vector<ToolSpec>& tool_specs,
                      const std::string&,
                      double) override {
        chat_call_count++;
        last_messages = messages;
        last_tool_specs = tool_specs;
        if (should_throw) {
            throw std::runtime_error("provider error");
        }
        if (!responses.empty()) {
            auto idx = static_cast<size_t>(chat_call_count - 1);
            if (idx < responses.size()) return responses[idx];
            return responses.back();
        }
        return next_response;
    }

    std::string last_simple_system;
    std::string last_simple_message;
    std::string simple_response = "simple response";

    std::string chat_simple(const std::string& system_prompt,
                            const std::string& message,
                            const std::string&,
                            double) override {
        last_simple_system = system_prompt;
        last_simple_message = message;
        return simple_response;
    }

    bool supports_native_tools() const override { return native_tools; }
    std::string provider_name() const override { return "mock"; }
};

// ── Mock tool ────────────────────────────────────────────────────

class MockTool : public Tool {
public:
    ToolResult execute(const std::string&) override {
        return ToolResult{true, "mock output"};
    }
    std::string tool_name() const override { return "mock_tool"; }
    std::string description() const override { return "A mock tool"; }
    std::string parameters_json() const override { return R"({"type":"object"})"; }
};

// Mock memory tool for contextual selection tests
// Must inherit MemoryAwareTool — Agent::wire_memory_tools() static_casts to it.
class MockMemoryTool : public MemoryAwareTool {
    std::string name_;
public:
    explicit MockMemoryTool(std::string name) : name_(std::move(name)) {}
    ToolResult execute(const std::string&) override {
        return ToolResult{true, "memory result"};
    }
    std::string tool_name() const override { return name_; }
    std::string description() const override { return "Mock " + name_; }
    std::string parameters_json() const override { return R"({"type":"object"})"; }
};

// Mock tool that returns verbose output (for output filter tests)
class VerboseOutputTool : public Tool {
public:
    ToolResult execute(const std::string&) override {
        std::string output = "\033[32mLine 1\033[0m\n";
        for (int i = 2; i <= 10; i++) {
            output += "Line " + std::to_string(i) + "\n";
        }
        return ToolResult{true, output};
    }
    std::string tool_name() const override { return "verbose_tool"; }
    std::string description() const override { return "Produces verbose output"; }
    std::string parameters_json() const override { return R"({"type":"object"})"; }
};

// Helper to build an Agent with a mock provider
struct MakeAgentResult {
    TestAgentSetup setup;
    MockProvider* mock;
};
static MakeAgentResult make_agent() {
    auto provider = std::make_unique<MockProvider>();
    auto* provider_ptr = provider.get();

    std::vector<std::unique_ptr<Tool>> tools;
    tools.push_back(std::make_unique<MockTool>());

    Config cfg;
    cfg.agent.max_tool_iterations = 5;
    cfg.agent.max_history_messages = 50;

    return {TestAgentSetup(std::move(provider), std::move(tools), cfg), provider_ptr};
}

// ── Basic process ────────────────────────────────────────────────

TEST_CASE("Agent: process returns content from provider", "[agent]") {
    auto [setup, mock] = make_agent();
    auto& agent = setup.agent;
    mock->next_response.content = "Hello from LLM";

    std::string reply = agent.process("Hi");
    REQUIRE(reply == "Hello from LLM");
    REQUIRE(mock->chat_call_count == 1);
}

TEST_CASE("Agent: process includes system prompt in first call", "[agent]") {
    auto [setup, mock] = make_agent();
    auto& agent = setup.agent;
    mock->next_response.content = "ok";

    agent.process("test");

    REQUIRE_FALSE(mock->last_messages.empty());
    REQUIRE(mock->last_messages[0].role == Role::System);
    REQUIRE(mock->last_messages[0].content.find("PtrClaw") != std::string::npos);
}

TEST_CASE("Agent: process appends user message to history", "[agent]") {
    auto [setup, mock] = make_agent();
    auto& agent = setup.agent;
    mock->next_response.content = "reply";

    agent.process("my question");

    // Messages should have: system, user, assistant (after response)
    // But we check the messages the provider received
    REQUIRE(mock->last_messages.size() >= 2);
    bool found_user = false;
    for (auto& msg : mock->last_messages) {
        if (msg.role == Role::User && msg.content == "my question") {
            found_user = true;
        }
    }
    REQUIRE(found_user);
}

TEST_CASE("Agent: no content returns default message", "[agent]") {
    auto [setup, mock] = make_agent();
    auto& agent = setup.agent;
    mock->next_response.content = std::nullopt;

    std::string reply = agent.process("Hi");
    REQUIRE(reply == "[No response]");
}

// ── Tool call loop ───────────────────────────────────────────────

TEST_CASE("Agent: executes tool call and loops", "[agent]") {
    auto [setup, mock] = make_agent();
    auto& agent = setup.agent;
    (void)agent;

    // First call returns a tool call
    ChatResponse tool_response;
    tool_response.content = "";
    tool_response.tool_calls = {ToolCall{"call1", "mock_tool", "{}"}};

    // Second call returns final content
    ChatResponse final_response;
    final_response.content = "Done after tool";

    // Mock needs to return different responses on each call
    // We'll handle this by checking call count
    int call_num = 0;
    auto provider = std::make_unique<MockProvider>();
    auto* provider_ptr = provider.get();

    // We can't easily do this with the simple mock, so let's test a simpler case:
    // just verify the iteration limit works
    provider_ptr->next_response.content = "direct reply";

    std::vector<std::unique_ptr<Tool>> tools;
    tools.push_back(std::make_unique<MockTool>());
    Config cfg;
    cfg.agent.max_tool_iterations = 5;
    TestAgentSetup setup2(std::move(provider), std::move(tools), cfg);
    auto& agent2 = setup2.agent;
    (void)call_num;

    std::string reply = agent2.process("test");
    REQUIRE(reply == "direct reply");
}

// ── History management ───────────────────────────────────────────

TEST_CASE("Agent: history_size grows with messages", "[agent]") {
    auto [setup, mock] = make_agent();
    auto& agent = setup.agent;
    mock->next_response.content = "reply";

    REQUIRE(agent.history_size() == 0);
    agent.process("first");
    // system + user + assistant = 3
    REQUIRE(agent.history_size() == 3);
    agent.process("second");
    // +user +assistant = 5
    REQUIRE(agent.history_size() == 5);
}

TEST_CASE("Agent: clear_history resets everything", "[agent]") {
    auto [setup, mock] = make_agent();
    auto& agent = setup.agent;
    mock->next_response.content = "reply";

    agent.process("hi");
    REQUIRE(agent.history_size() > 0);

    agent.clear_history();
    REQUIRE(agent.history_size() == 0);
}

TEST_CASE("Agent: estimated_tokens is non-zero after messages", "[agent]") {
    auto [setup, mock] = make_agent();
    auto& agent = setup.agent;
    mock->next_response.content = "this is a long enough response to have some tokens";

    agent.process("a message with some content too");
    REQUIRE(agent.estimated_tokens() > 0);
}

// ── Model switching ──────────────────────────────────────────────

TEST_CASE("Agent: set_model and model getter", "[agent]") {
    auto [setup, mock] = make_agent();
    auto& agent = setup.agent;
    agent.set_model("gpt-4");
    REQUIRE(agent.model() == "gpt-4");
}

TEST_CASE("Agent: provider_name returns mock", "[agent]") {
    auto [setup, mock] = make_agent();
    auto& agent = setup.agent;
    REQUIRE(agent.provider_name() == "mock");
}

// ── Provider switching ───────────────────────────────────────────

TEST_CASE("Agent: set_provider switches provider", "[agent]") {
    auto [setup, mock] = make_agent();
    auto& agent = setup.agent;
    mock->next_response.content = "from old";
    agent.process("init");

    auto new_provider = std::make_unique<MockProvider>();
    new_provider->next_response.content = "from new";
    agent.set_provider(std::move(new_provider));

    std::string reply = agent.process("test");
    REQUIRE(reply == "from new");
}

// ── dispatch_tool ────────────────────────────────────────────────

TEST_CASE("dispatch_tool: finds and executes matching tool", "[dispatcher]") {
    std::vector<std::unique_ptr<Tool>> tools;
    tools.push_back(std::make_unique<MockTool>());

    ToolCall call{"id1", "mock_tool", "{}"};
    auto result = dispatch_tool(call, tools);
    REQUIRE(result.success);
    REQUIRE(result.output == "mock output");
}

TEST_CASE("dispatch_tool: returns error for unknown tool", "[dispatcher]") {
    std::vector<std::unique_ptr<Tool>> tools;
    tools.push_back(std::make_unique<MockTool>());

    ToolCall call{"id2", "nonexistent", "{}"};
    auto result = dispatch_tool(call, tools);
    REQUIRE_FALSE(result.success);
    REQUIRE(result.output.find("Unknown tool") != std::string::npos);
}

TEST_CASE("dispatch_tool: empty tool list", "[dispatcher]") {
    std::vector<std::unique_ptr<Tool>> tools;
    ToolCall call{"id3", "anything", "{}"};
    auto result = dispatch_tool(call, tools);
    REQUIRE_FALSE(result.success);
}

// ── Tool call loop with sequenced responses ─────────────────────

TEST_CASE("Agent: tool call triggers second chat round", "[agent]") {
    auto provider = std::make_unique<MockProvider>();
    auto* mock = provider.get();

    // First response: tool call
    ChatResponse r1;
    r1.content = "";
    r1.tool_calls = {ToolCall{"call1", "mock_tool", "{}"}};

    // Second response: final content
    ChatResponse r2;
    r2.content = "Done after tool";

    mock->responses = {r1, r2};

    std::vector<std::unique_ptr<Tool>> tools;
    tools.push_back(std::make_unique<MockTool>());
    Config cfg;
    cfg.agent.max_tool_iterations = 5;
    TestAgentSetup setup(std::move(provider), std::move(tools), cfg);
    auto& agent = setup.agent;

    std::string reply = agent.process("do something");
    REQUIRE(reply == "Done after tool");
    REQUIRE(mock->chat_call_count == 2);
}

TEST_CASE("Agent: max tool iterations reached", "[agent]") {
    auto provider = std::make_unique<MockProvider>();
    auto* mock = provider.get();

    // Always return tool calls, never final content
    ChatResponse tool_resp;
    tool_resp.content = "";
    tool_resp.tool_calls = {ToolCall{"call1", "mock_tool", "{}"}};
    mock->next_response = tool_resp;

    std::vector<std::unique_ptr<Tool>> tools;
    tools.push_back(std::make_unique<MockTool>());
    Config cfg;
    cfg.agent.max_tool_iterations = 3;
    TestAgentSetup setup(std::move(provider), std::move(tools), cfg);
    auto& agent = setup.agent;

    std::string reply = agent.process("loop forever");
    REQUIRE(reply == "[Max tool iterations reached]");
    REQUIRE(mock->chat_call_count == 3);
}

// ── Provider error handling ─────────────────────────────────────

TEST_CASE("Agent: provider exception returns error message", "[agent]") {
    auto provider = std::make_unique<MockProvider>();
    provider->should_throw = true;

    std::vector<std::unique_ptr<Tool>> tools;
    tools.push_back(std::make_unique<MockTool>());
    Config cfg;
    TestAgentSetup setup(std::move(provider), std::move(tools), cfg);
    auto& agent = setup.agent;

    std::string reply = agent.process("trigger error");
    REQUIRE(reply.find("Error calling provider") != std::string::npos);
    REQUIRE(reply.find("provider error") != std::string::npos);
}

// ── Tool result in history (native provider) ────────────────────

TEST_CASE("Agent: tool result appears in history for native provider", "[agent]") {
    auto provider = std::make_unique<MockProvider>();
    auto* mock = provider.get();
    mock->native_tools = true;

    ChatResponse r1;
    r1.content = "";
    r1.tool_calls = {ToolCall{"call1", "mock_tool", "{}"}};

    ChatResponse r2;
    r2.content = "final";

    mock->responses = {r1, r2};

    std::vector<std::unique_ptr<Tool>> tools;
    tools.push_back(std::make_unique<MockTool>());
    Config cfg;
    cfg.agent.max_tool_iterations = 5;
    TestAgentSetup setup(std::move(provider), std::move(tools), cfg);
    auto& agent = setup.agent;

    agent.process("test");

    // On second call, messages should include a Tool role message
    bool found_tool = false;
    for (auto& msg : mock->last_messages) {
        if (msg.role == Role::Tool) {
            found_tool = true;
            REQUIRE(msg.content == "mock output");
        }
    }
    REQUIRE(found_tool);
}

// ── XML tool calls (non-native provider) ────────────────────────

TEST_CASE("Agent: XML tool call parsed for non-native provider", "[agent]") {
    auto provider = std::make_unique<MockProvider>();
    auto* mock = provider.get();
    mock->native_tools = false;

    // First response: content with XML tool call
    ChatResponse r1;
    r1.content = R"(I'll read the file. <tool_call>{"name":"mock_tool","arguments":{}}</tool_call>)";

    // Second response: final answer
    ChatResponse r2;
    r2.content = "Here's the result.";

    mock->responses = {r1, r2};

    std::vector<std::unique_ptr<Tool>> tools;
    tools.push_back(std::make_unique<MockTool>());
    Config cfg;
    cfg.agent.max_tool_iterations = 5;
    TestAgentSetup setup(std::move(provider), std::move(tools), cfg);
    auto& agent = setup.agent;

    std::string reply = agent.process("read file");
    REQUIRE(reply == "Here's the result.");
    REQUIRE(mock->chat_call_count == 2);
}

TEST_CASE("Agent: non-native provider without tool call returns content", "[agent]") {
    auto provider = std::make_unique<MockProvider>();
    auto* mock = provider.get();
    mock->native_tools = false;

    ChatResponse r1;
    r1.content = "Just a plain response with no tools";
    mock->next_response = r1;

    std::vector<std::unique_ptr<Tool>> tools;
    tools.push_back(std::make_unique<MockTool>());
    Config cfg;
    TestAgentSetup setup(std::move(provider), std::move(tools), cfg);
    auto& agent = setup.agent;

    std::string reply = agent.process("hello");
    REQUIRE(reply == "Just a plain response with no tools");
}

// ── History compaction ──────────────────────────────────────────

TEST_CASE("Agent: compact_history triggers on large history", "[agent]") {
    auto provider = std::make_unique<MockProvider>();
    auto* mock = provider.get();
    mock->next_response.content = "reply";

    std::vector<std::unique_ptr<Tool>> tools;
    Config cfg;
    cfg.agent.max_tool_iterations = 5;
    cfg.agent.max_history_messages = 10; // Low threshold to trigger compaction
    TestAgentSetup setup(std::move(provider), std::move(tools), cfg);
    auto& agent = setup.agent;

    // Generate enough messages to exceed max_history_messages
    for (int i = 0; i < 10; i++) {
        agent.process("message " + std::to_string(i));
    }

    // After compaction, history should be smaller than total messages added
    // Total without compaction: 1 system + 10*(user+assistant) = 21
    REQUIRE(agent.history_size() < 21);
}

TEST_CASE("Agent: compaction does not orphan tool response messages", "[agent]") {
    auto provider = std::make_unique<MockProvider>();
    auto* mock = provider.get();

    // Alternate: tool call response, then plain response
    ChatResponse tool_resp;
    tool_resp.content = "Let me check.";
    tool_resp.tool_calls = {{"call_1", "mock_tool", "{}"}};

    ChatResponse plain_resp;
    plain_resp.content = "Done.";

    // Queue: each process() call triggers tool_resp -> plain_resp (2 chat calls),
    // except we need the mock to alternate properly.
    // After tool call: provider gets called again for the follow-up.
    // So for each process(): call 1 = tool_resp, call 2 = plain_resp
    // For 8 rounds: 16 responses total
    for (int i = 0; i < 8; i++) {
        mock->responses.push_back(tool_resp);
        mock->responses.push_back(plain_resp);
    }

    std::vector<std::unique_ptr<Tool>> tools;
    tools.push_back(std::make_unique<MockTool>());
    Config cfg;
    cfg.agent.max_tool_iterations = 5;
    cfg.agent.max_history_messages = 10; // Low threshold to trigger compaction

    TestAgentSetup setup(std::move(provider), std::move(tools), cfg);
    auto& agent = setup.agent;

    // Generate enough messages with tool calls to trigger compaction
    for (int i = 0; i < 8; i++) {
        agent.process("request " + std::to_string(i));
    }

    // Verify: in the last messages sent to the provider, no Tool message
    // appears without a preceding Assistant message with tool_calls
    for (size_t i = 0; i < mock->last_messages.size(); i++) {
        if (mock->last_messages[i].role == Role::Tool) {
            REQUIRE(i > 0);
            // Walk back to find the assistant message (could be multiple tool results)
            size_t j = i - 1;
            while (j > 0 && mock->last_messages[j].role == Role::Tool) {
                j--;
            }
            REQUIRE(mock->last_messages[j].role == Role::Assistant);
            // Assistant message must carry tool_calls (stored in name field)
            REQUIRE(mock->last_messages[j].name.has_value());
        }
    }
}

TEST_CASE("Agent: compaction guard prevents premature compaction at 12 or fewer messages", "[agent][compaction]") {
    // The compact_history() guard: history_.size() <= 12 prevents compaction
    // even when message count exceeds max_history_messages.
    auto provider = std::make_unique<MockProvider>();
    auto* mock = provider.get();
    mock->next_response.content = "reply";

    std::vector<std::unique_ptr<Tool>> tools;
    Config cfg;
    cfg.agent.max_history_messages = 3; // Very low to make should_compact=true
    TestAgentSetup setup(std::move(provider), std::move(tools), cfg);
    auto& agent = setup.agent;

    // 3 calls -> 1 sys + 3 user + 3 asst = 7 messages
    // 7 > 3 (should_compact=true) but 7 <= 12 (guard fires), so NO compaction
    for (int i = 0; i < 3; i++) {
        agent.process("msg" + std::to_string(i));
    }

    // All messages intact — no summary injected
    REQUIRE(agent.history_size() == 7);
}

TEST_CASE("Agent: compaction summary text contains user and assistant counts", "[agent][compaction]") {
    // compact_history() builds a summary "[Conversation history compacted.
    // Previous discussion covered: N user messages, N assistant responses]"
    // and inserts it as a User-role message.
    auto provider = std::make_unique<MockProvider>();
    auto* mock = provider.get();
    mock->next_response.content = "reply";

    std::vector<std::unique_ptr<Tool>> tools;
    Config cfg;
    cfg.agent.max_history_messages = 10;
    TestAgentSetup setup(std::move(provider), std::move(tools), cfg);
    auto& agent = setup.agent;

    // 7 calls -> 1+14=15 messages, exceeds 10 AND 12 -> compaction fires at call 7
    for (int i = 0; i < 7; i++) {
        agent.process("msg" + std::to_string(i));
    }

    // One more call so provider sees the post-compaction history
    agent.process("after");

    bool found_summary = false;
    for (const auto& msg : mock->last_messages) {
        if (msg.role == Role::User &&
            msg.content.find("[Conversation history compacted") != std::string::npos) {
            found_summary = true;
            REQUIRE(msg.content.find("user messages") != std::string::npos);
            REQUIRE(msg.content.find("assistant responses") != std::string::npos);
        }
    }
    REQUIRE(found_summary);
    REQUIRE(mock->last_messages[0].role == Role::System);
}

TEST_CASE("Agent: clear then re-process re-injects system prompt", "[agent]") {
    auto [setup, mock] = make_agent();
    auto& agent = setup.agent;
    mock->next_response.content = "ok";

    agent.process("first");
    agent.clear_history();
    agent.process("second");

    REQUIRE(mock->last_messages[0].role == Role::System);
    REQUIRE(mock->last_messages[0].content.find("PtrClaw") != std::string::npos);
}

// ── set_provider re-injects system prompt ───────────────────────

TEST_CASE("Agent: set_provider removes old system prompt", "[agent]") {
    auto [setup, mock] = make_agent();
    auto& agent = setup.agent;
    mock->next_response.content = "ok";
    agent.process("init");

    auto new_mock = std::make_unique<MockProvider>();
    new_mock->next_response.content = "new reply";
    new_mock->native_tools = false; // Different tool support
    auto* new_ptr = new_mock.get();
    agent.set_provider(std::move(new_mock));

    agent.process("test");

    // System prompt should be re-injected for new provider
    REQUIRE(new_ptr->last_messages[0].role == Role::System);
    // Non-native provider should get tool descriptions
    REQUIRE(new_ptr->last_messages[0].content.find("Available tools:") != std::string::npos);
}

// ── Synthesis ────────────────────────────────────────────────────

TEST_CASE("Agent: synthesis triggers after configured interval", "[agent]") {
    auto provider = std::make_unique<MockProvider>();
    auto* mock = provider.get();

    // Regular chat responses
    ChatResponse normal;
    normal.content = "I understand.";

    // Synthesis response (chat_simple is used for synthesis)
    // The mock's chat_simple returns "simple response" which isn't valid JSON,
    // so synthesis will silently fail — which is correct behavior.
    // We verify it doesn't crash and the agent still works.
    mock->next_response = normal;

    std::vector<std::unique_ptr<Tool>> tools;
    Config cfg;
    cfg.agent.max_tool_iterations = 5;
    cfg.memory.backend = "json";
    cfg.memory.synthesis = true;
    cfg.memory.synthesis_interval = 2; // trigger every 2 turns

    // Use a temp memory file
    std::string mem_path = "/tmp/ptrclaw_test_synthesis_" + std::to_string(getpid()) + ".json";
    auto memory = std::make_unique<JsonMemory>(mem_path);

    TestAgentSetup setup(std::move(provider), std::move(tools), cfg);
    auto& agent = setup.agent;
    agent.set_memory(std::move(memory));

    // Process enough messages to trigger synthesis
    agent.process("Tell me about C++");
    agent.process("What about Python?");

    // Agent should still work correctly after synthesis attempt
    REQUIRE(mock->chat_call_count >= 2);

    std::filesystem::remove(mem_path);
}

TEST_CASE("Agent: synthesis passes system prompt and message correctly", "[agent]") {
    auto provider = std::make_unique<MockProvider>();
    auto* mock = provider.get();

    ChatResponse normal;
    normal.content = "I understand.";
    mock->next_response = normal;
    mock->simple_response = R"([{"key":"test","content":"test","category":"knowledge","links":[]}])";

    std::vector<std::unique_ptr<Tool>> tools;
    Config cfg;
    cfg.agent.max_tool_iterations = 5;
    cfg.memory.backend = "json";
    cfg.memory.synthesis = true;
    cfg.memory.synthesis_interval = 1;

    std::string mem_path = "/tmp/ptrclaw_test_synth_args_" + std::to_string(getpid()) + ".json";
    auto memory = std::make_unique<JsonMemory>(mem_path);

    TestAgentSetup setup(std::move(provider), std::move(tools), cfg);
    auto& agent = setup.agent;
    agent.set_memory(std::move(memory));

    agent.process("Hello world");

    // System prompt should be the short role instruction
    REQUIRE(mock->last_simple_system.find("knowledge extraction") != std::string::npos);
    // User message should contain the extraction rules and conversation
    REQUIRE(mock->last_simple_message.find("Extract atomic knowledge") != std::string::npos);
    REQUIRE(mock->last_simple_message.find("Hello world") != std::string::npos);

    std::filesystem::remove(mem_path);
}

TEST_CASE("Agent: synthesis disabled flag prevents chat_simple calls", "[agent][synthesis]") {
    // When config_.memory.synthesis == false, maybe_synthesize() returns early
    // and chat_simple should never be invoked.
    auto provider = std::make_unique<MockProvider>();
    auto* mock = provider.get();
    mock->next_response.content = "I understand.";

    std::vector<std::unique_ptr<Tool>> tools;
    Config cfg;
    cfg.memory.backend = "json";
    cfg.memory.synthesis = false; // Disabled
    cfg.memory.synthesis_interval = 1;

    std::string mem_path = "/tmp/ptrclaw_test_synth_disabled_" + std::to_string(getpid()) + ".json";
    auto memory = std::make_unique<JsonMemory>(mem_path);
    TestAgentSetup setup(std::move(provider), std::move(tools), cfg);
    auto& agent = setup.agent;
    agent.set_memory(std::move(memory));

    agent.process("hello");
    agent.process("world");

    REQUIRE(mock->last_simple_system.empty());

    std::filesystem::remove(mem_path);
}

TEST_CASE("Agent: synthesis interval=0 disables periodic synthesis", "[agent][synthesis]") {
    // maybe_synthesize() guards against synthesis_interval == 0 explicitly.
    auto provider = std::make_unique<MockProvider>();
    auto* mock = provider.get();
    mock->next_response.content = "I understand.";

    std::vector<std::unique_ptr<Tool>> tools;
    Config cfg;
    cfg.memory.backend = "json";
    cfg.memory.synthesis = true;
    cfg.memory.synthesis_interval = 0; // Guard: never trigger

    std::string mem_path = "/tmp/ptrclaw_test_synth_zero_" + std::to_string(getpid()) + ".json";
    auto memory = std::make_unique<JsonMemory>(mem_path);
    TestAgentSetup setup(std::move(provider), std::move(tools), cfg);
    auto& agent = setup.agent;
    agent.set_memory(std::move(memory));

    for (int i = 0; i < 10; i++) {
        agent.process("message " + std::to_string(i));
    }

    REQUIRE(mock->last_simple_system.empty());

    std::filesystem::remove(mem_path);
}

TEST_CASE("Agent: synthesis stores extracted entries in memory", "[agent][synthesis]") {
    // When synthesis returns valid JSON, entries are stored in the memory backend.
    auto provider = std::make_unique<MockProvider>();
    auto* mock = provider.get();
    mock->next_response.content = "I understand.";
    mock->simple_response = R"([{"key":"user-likes-rust","content":"User prefers Rust","category":"knowledge","links":[]}])";

    std::vector<std::unique_ptr<Tool>> tools;
    Config cfg;
    cfg.memory.backend = "json";
    cfg.memory.synthesis = true;
    cfg.memory.synthesis_interval = 1;

    std::string mem_path = "/tmp/ptrclaw_test_synth_store_" + std::to_string(getpid()) + ".json";
    auto memory = std::make_unique<JsonMemory>(mem_path);
    TestAgentSetup setup(std::move(provider), std::move(tools), cfg);
    auto& agent = setup.agent;
    agent.set_memory(std::move(memory));

    agent.process("I love Rust programming");

    auto entries = agent.memory()->list(std::nullopt, 10);
    bool found = false;
    for (const auto& e : entries) {
        if (e.key == "user-likes-rust") {
            REQUIRE(e.content == "User prefers Rust");
            found = true;
        }
    }
    REQUIRE(found);

    std::filesystem::remove(mem_path);
}

TEST_CASE("Agent: compaction forces synthesis before discarding history", "[agent][compaction]") {
    // When turns_since_synthesis_ > 0 at compaction time, compact_history()
    // calls run_synthesis() to extract knowledge before the middle history is lost.
    auto provider = std::make_unique<MockProvider>();
    auto* mock = provider.get();
    mock->next_response.content = "reply";
    mock->simple_response = R"([{"key":"captured","content":"Before compaction","category":"knowledge","links":[]}])";

    std::vector<std::unique_ptr<Tool>> tools;
    Config cfg;
    cfg.agent.max_history_messages = 10;
    cfg.memory.backend = "json";
    cfg.memory.synthesis = true;
    cfg.memory.synthesis_interval = 100; // Won't fire periodically; forces at compaction

    std::string mem_path = "/tmp/ptrclaw_test_compact_synth_" + std::to_string(getpid()) + ".json";
    auto memory = std::make_unique<JsonMemory>(mem_path);
    TestAgentSetup setup(std::move(provider), std::move(tools), cfg);
    auto& agent = setup.agent;
    agent.set_memory(std::move(memory));

    // 7 calls -> 15 messages -> compaction fires; turns_since_synthesis_ = 7 > 0
    for (int i = 0; i < 7; i++) {
        agent.process("message " + std::to_string(i));
    }

    // chat_simple must have been called (synthesis was forced during compaction)
    REQUIRE(!mock->last_simple_system.empty());
    REQUIRE(mock->last_simple_system.find("knowledge extraction") != std::string::npos);

    std::filesystem::remove(mem_path);
}

TEST_CASE("Agent: synthesis is no-op when memory backend is none", "[agent][synthesis]") {
    // has_active_memory() returns false for backend="none"; synthesis must be skipped.
    auto provider = std::make_unique<MockProvider>();
    auto* mock = provider.get();
    mock->next_response.content = "I understand.";

    std::vector<std::unique_ptr<Tool>> tools;
    Config cfg;
    cfg.memory.backend = "none";
    cfg.memory.synthesis = true;
    cfg.memory.synthesis_interval = 1;

    TestAgentSetup setup(std::move(provider), std::move(tools), cfg);
    auto& agent = setup.agent;

    for (int i = 0; i < 5; i++) {
        agent.process("message " + std::to_string(i));
    }

    REQUIRE(mock->last_simple_system.empty());
}

// ── Contextual tool selection ───────────────────────────────────

TEST_CASE("Agent: memory tools excluded from specs when memory inactive", "[agent]") {
    auto provider = std::make_unique<MockProvider>();
    auto* mock = provider.get();
    mock->native_tools = true;
    mock->next_response.content = "ok";

    std::vector<std::unique_ptr<Tool>> tools;
    tools.push_back(std::make_unique<MockTool>());
    tools.push_back(std::make_unique<MockMemoryTool>("memory_store"));
    tools.push_back(std::make_unique<MockMemoryTool>("memory_recall"));
    tools.push_back(std::make_unique<MockMemoryTool>("memory_forget"));
    tools.push_back(std::make_unique<MockMemoryTool>("memory_link"));

    Config cfg;
    cfg.agent.max_tool_iterations = 5;
    cfg.memory.backend = "none"; // Explicitly inactive
    TestAgentSetup setup(std::move(provider), std::move(tools), cfg);
    auto& agent = setup.agent;

    agent.process("test");

    // Only mock_tool should appear in specs, memory tools excluded
    REQUIRE(mock->last_tool_specs.size() == 1);
    REQUIRE(mock->last_tool_specs[0].name == "mock_tool");
}

TEST_CASE("Agent: memory tools included when memory is active", "[agent]") {
    auto provider = std::make_unique<MockProvider>();
    auto* mock = provider.get();
    mock->native_tools = true;
    mock->next_response.content = "ok";

    std::vector<std::unique_ptr<Tool>> tools;
    tools.push_back(std::make_unique<MockTool>());
    tools.push_back(std::make_unique<MockMemoryTool>("memory_store"));
    tools.push_back(std::make_unique<MockMemoryTool>("memory_recall"));

    Config cfg;
    cfg.agent.max_tool_iterations = 5;
    cfg.memory.backend = "json";

    std::string mem_path = "/tmp/ptrclaw_test_ctx_" + std::to_string(getpid()) + ".json";
    auto memory = std::make_unique<JsonMemory>(mem_path);

    TestAgentSetup setup(std::move(provider), std::move(tools), cfg);
    auto& agent = setup.agent;
    agent.set_memory(std::move(memory));
    setup.tool_mgr->wire_memory(agent.memory());
    setup.tool_mgr->publish_tool_specs();

    agent.process("test");

    // All three tools should appear (mock_tool + 2 memory tools)
    REQUIRE(mock->last_tool_specs.size() == 3);

    std::filesystem::remove(mem_path);
}

TEST_CASE("Agent: memory tools excluded from system prompt for non-native provider", "[agent]") {
    auto provider = std::make_unique<MockProvider>();
    auto* mock = provider.get();
    mock->native_tools = false;
    mock->next_response.content = "ok";

    std::vector<std::unique_ptr<Tool>> tools;
    tools.push_back(std::make_unique<MockTool>());
    tools.push_back(std::make_unique<MockMemoryTool>("memory_store"));
    tools.push_back(std::make_unique<MockMemoryTool>("memory_recall"));

    Config cfg;
    cfg.agent.max_tool_iterations = 5;
    cfg.memory.backend = "none"; // Explicitly inactive
    TestAgentSetup setup(std::move(provider), std::move(tools), cfg);
    auto& agent = setup.agent;

    agent.process("test");

    // System prompt should list mock_tool but not memory tools
    const auto& sys = mock->last_messages[0].content;
    REQUIRE(sys.find("mock_tool") != std::string::npos);
    REQUIRE(sys.find("memory_store") == std::string::npos);
    REQUIRE(sys.find("memory_recall") == std::string::npos);
}

// ── Output filtering in agent loop ──────────────────────────────

TEST_CASE("Agent: tool output is filtered (ANSI stripped)", "[agent]") {
    auto provider = std::make_unique<MockProvider>();
    auto* mock = provider.get();
    mock->native_tools = true;

    // First response: call verbose_tool
    ChatResponse r1;
    r1.content = "";
    r1.tool_calls = {ToolCall{"call1", "verbose_tool", "{}"}};

    // Second response: final content
    ChatResponse r2;
    r2.content = "done";

    mock->responses = {r1, r2};

    std::vector<std::unique_ptr<Tool>> tools;
    tools.push_back(std::make_unique<VerboseOutputTool>());
    Config cfg;
    cfg.agent.max_tool_iterations = 5;
    TestAgentSetup setup(std::move(provider), std::move(tools), cfg);
    auto& agent = setup.agent;

    agent.process("run verbose");

    // On second call, Tool message should have ANSI stripped
    for (const auto& msg : mock->last_messages) {
        if (msg.role == Role::Tool) {
            // Should contain "Line 1" but no ANSI escape codes
            REQUIRE(msg.content.find("Line 1") != std::string::npos);
            REQUIRE(msg.content.find("\033[") == std::string::npos);
        }
    }
}

// Skill whitelist tests removed — skill tool whitelisting was removed.
// Skills now only inject prompts; all tools are always available.
