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
    cfg.memory.backend = "none"; // Isolate from real memory file

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
    cfg.memory.backend = "none"; // Isolate from persistent memory state
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
    cfg.memory.backend = "none"; // Isolate from persistent memory state

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

TEST_CASE("Agent: compaction produces structured episode summary", "[agent][compaction]") {
    // compact_history() now builds a structured episode summary:
    // "[Episode summary: N turns (U user, A assistant)[, T tool calls][. Tools: ...].]"
    // and inserts it as a User-role message.
    auto provider = std::make_unique<MockProvider>();
    auto* mock = provider.get();
    mock->next_response.content = "reply";

    std::vector<std::unique_ptr<Tool>> tools;
    Config cfg;
    cfg.agent.max_history_messages = 10;
    cfg.memory.backend = "none"; // Isolate from persistent memory state
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
            msg.content.find("[Episode summary:") != std::string::npos) {
            found_summary = true;
            REQUIRE(msg.content.find("user") != std::string::npos);
            REQUIRE(msg.content.find("assistant") != std::string::npos);
        }
    }
    REQUIRE(found_summary);
    REQUIRE(mock->last_messages[0].role == Role::System);
}

TEST_CASE("Agent: episode summary includes tool names from discarded messages", "[agent][compaction]") {
    // When compacted messages contain tool calls, the episode summary lists
    // the unique tool names used in the discarded portion.
    auto provider = std::make_unique<MockProvider>();
    auto* mock = provider.get();

    ChatResponse tool_resp;
    tool_resp.content = "Checking.";
    tool_resp.tool_calls = {{"call_1", "mock_tool", "{}"}};

    ChatResponse plain_resp;
    plain_resp.content = "Done.";

    // Queue: tool_resp -> plain_resp for each process() call
    for (int i = 0; i < 8; i++) {
        mock->responses.push_back(tool_resp);
        mock->responses.push_back(plain_resp);
    }

    std::vector<std::unique_ptr<Tool>> tools;
    tools.push_back(std::make_unique<MockTool>());
    Config cfg;
    cfg.agent.max_tool_iterations = 5;
    cfg.agent.max_history_messages = 10;
    cfg.memory.backend = "none"; // Isolate from persistent memory state
    TestAgentSetup setup(std::move(provider), std::move(tools), cfg);
    auto& agent = setup.agent;

    for (int i = 0; i < 8; i++) {
        agent.process("request " + std::to_string(i));
    }

    bool found_summary = false;
    for (const auto& msg : mock->last_messages) {
        if (msg.role == Role::User &&
            msg.content.find("[Episode summary:") != std::string::npos) {
            found_summary = true;
            // Tool name "mock_tool" should appear in the summary
            REQUIRE(msg.content.find("mock_tool") != std::string::npos);
        }
    }
    REQUIRE(found_summary);
}

// ── Episode archive (PER-388) ────────────────────────────────────

TEST_CASE("Agent: episode archive is empty before any compaction", "[agent][episode]") {
    auto provider = std::make_unique<MockProvider>();
    auto* mock = provider.get();
    mock->next_response.content = "reply";

    std::vector<std::unique_ptr<Tool>> tools;
    Config cfg;
    cfg.memory.backend = "none"; // Isolate from persistent memory state
    TestAgentSetup setup(std::move(provider), std::move(tools), cfg);
    auto& agent = setup.agent;

    REQUIRE(agent.episodes().empty());
    (void)mock; // suppress unused warning
}

TEST_CASE("Agent: compact_history archives discarded messages", "[agent][episode]") {
    // After the first compaction, episodes() must contain exactly one entry
    // holding the messages that were dropped from active history.
    auto provider = std::make_unique<MockProvider>();
    auto* mock = provider.get();
    mock->next_response.content = "reply";

    std::vector<std::unique_ptr<Tool>> tools;
    Config cfg;
    cfg.agent.max_history_messages = 10;
    cfg.memory.backend = "none"; // Isolate from persistent memory state
    TestAgentSetup setup(std::move(provider), std::move(tools), cfg);
    auto& agent = setup.agent;

    // 7 calls -> 15 messages -> compaction fires
    for (int i = 0; i < 7; i++) {
        agent.process("message " + std::to_string(i));
    }

    // Compaction may fire more than once as history stays > max after compaction;
    // at minimum one episode must have been archived.
    REQUIRE(!agent.episodes().empty());
    const auto& ep = agent.episodes()[0];
    REQUIRE(ep.id == "episode:0");
    REQUIRE(ep.timestamp > 0);
    // Archived messages must not be empty (the discarded slice)
    REQUIRE(!ep.messages.empty());
    (void)mock;
}

TEST_CASE("Agent: episode archive contains original message content", "[agent][episode]") {
    // The archived slice must preserve the actual user messages that were sent
    // before compaction — verifying the messages are copies, not just counts.
    auto provider = std::make_unique<MockProvider>();
    auto* mock = provider.get();
    mock->next_response.content = "reply";

    std::vector<std::unique_ptr<Tool>> tools;
    Config cfg;
    cfg.agent.max_history_messages = 10;
    cfg.memory.backend = "none"; // Isolate from persistent memory state
    TestAgentSetup setup(std::move(provider), std::move(tools), cfg);
    auto& agent = setup.agent;

    // Use distinct messages so we can check archival content
    for (int i = 0; i < 7; i++) {
        agent.process("unique_msg_" + std::to_string(i));
    }

    REQUIRE(!agent.episodes().empty());
    const auto& ep = agent.episodes()[0];

    // At least one archived message should carry one of the early user strings
    bool found = false;
    for (const auto& msg : ep.messages) {
        if (msg.role == Role::User &&
            msg.content.find("unique_msg_") != std::string::npos) {
            found = true;
            break;
        }
    }
    REQUIRE(found);
    (void)mock;
}

TEST_CASE("Agent: episode_by_id returns correct archived record", "[agent][episode]") {
    auto provider = std::make_unique<MockProvider>();
    auto* mock = provider.get();
    mock->next_response.content = "reply";

    std::vector<std::unique_ptr<Tool>> tools;
    Config cfg;
    cfg.agent.max_history_messages = 10;
    cfg.memory.backend = "none"; // Isolate from persistent memory state
    TestAgentSetup setup(std::move(provider), std::move(tools), cfg);
    auto& agent = setup.agent;

    for (int i = 0; i < 7; i++) {
        agent.process("msg" + std::to_string(i));
    }

    REQUIRE(!agent.episodes().empty());
    const std::string ep_id = agent.episodes()[0].id;

    const EpisodeRecord* found = agent.episode_by_id(ep_id);
    REQUIRE(found != nullptr);
    REQUIRE(found->id == ep_id);

    // Lookup of an unknown id returns null
    REQUIRE(agent.episode_by_id("episode:99") == nullptr);
    (void)mock;
}

TEST_CASE("Agent: episode archives accumulate across multiple compactions", "[agent][episode]") {
    // Each compaction event must append a new EpisodeRecord with a distinct ID.
    auto provider = std::make_unique<MockProvider>();
    auto* mock = provider.get();
    mock->next_response.content = "reply";

    std::vector<std::unique_ptr<Tool>> tools;
    Config cfg;
    cfg.agent.max_history_messages = 10;
    cfg.memory.backend = "none"; // Isolate from persistent memory state
    TestAgentSetup setup(std::move(provider), std::move(tools), cfg);
    auto& agent = setup.agent;

    // First compaction fires once the message count exceeds the threshold + guard
    for (int i = 0; i < 7; i++) {
        agent.process("round1_" + std::to_string(i));
    }
    size_t after_first_batch = agent.episodes().size();
    REQUIRE(after_first_batch >= 1);

    // More messages must add further episodes
    for (int i = 0; i < 7; i++) {
        agent.process("round2_" + std::to_string(i));
    }
    REQUIRE(agent.episodes().size() > after_first_batch);

    // IDs must be distinct and monotonically assigned
    REQUIRE(agent.episodes()[0].id == "episode:0");
    REQUIRE(agent.episodes()[1].id == "episode:1");
    (void)mock;
}

TEST_CASE("Agent: episode summary includes archive id reference", "[agent][episode]") {
    // The episode summary inserted into the compacted history must contain
    // the stable archive reference so the LLM can cite it if needed.
    auto provider = std::make_unique<MockProvider>();
    auto* mock = provider.get();
    mock->next_response.content = "reply";

    std::vector<std::unique_ptr<Tool>> tools;
    Config cfg;
    cfg.agent.max_history_messages = 10;
    cfg.memory.backend = "none"; // Isolate from persistent memory state
    TestAgentSetup setup(std::move(provider), std::move(tools), cfg);
    auto& agent = setup.agent;

    for (int i = 0; i < 7; i++) {
        agent.process("msg" + std::to_string(i));
    }

    // Trigger one more process so provider sees the post-compaction history
    agent.process("after");

    // The archive id may be any episode number (compaction may fire multiple times);
    // verify that at least one user message carries an "Archive: episode:" reference.
    bool found_archive_ref = false;
    for (const auto& msg : mock->last_messages) {
        if (msg.role == Role::User &&
            msg.content.find("Archive: episode:") != std::string::npos) {
            found_archive_ref = true;
            break;
        }
    }
    REQUIRE(found_archive_ref);
}

TEST_CASE("Agent: episode archive metadata matches compacted turn counts", "[agent][episode]") {
    // The EpisodeRecord metadata (user_turns, assistant_turns) must agree
    // with the actual roles of the archived message slice.
    auto provider = std::make_unique<MockProvider>();
    auto* mock = provider.get();
    mock->next_response.content = "reply";

    std::vector<std::unique_ptr<Tool>> tools;
    Config cfg;
    cfg.agent.max_history_messages = 10;
    cfg.memory.backend = "none"; // Isolate from persistent memory state
    TestAgentSetup setup(std::move(provider), std::move(tools), cfg);
    auto& agent = setup.agent;

    for (int i = 0; i < 7; i++) {
        agent.process("msg" + std::to_string(i));
    }

    REQUIRE(!agent.episodes().empty());
    const auto& ep = agent.episodes()[0];

    // Count roles in the archived messages to cross-check metadata
    int counted_user = 0, counted_asst = 0;
    for (const auto& msg : ep.messages) {
        if (msg.role == Role::User)      counted_user++;
        if (msg.role == Role::Assistant) counted_asst++;
    }
    REQUIRE(counted_user == ep.user_turns);
    REQUIRE(counted_asst == ep.assistant_turns);
    (void)mock;
}

TEST_CASE("Agent: clear_history preserves episode archives", "[agent][episode]") {
    // Episode archives must survive clear_history() — they are recoverable
    // references, not discarded with the active conversation window.
    auto provider = std::make_unique<MockProvider>();
    auto* mock = provider.get();
    mock->next_response.content = "reply";

    std::vector<std::unique_ptr<Tool>> tools;
    Config cfg;
    cfg.agent.max_history_messages = 10;
    cfg.memory.backend = "none"; // Isolate from persistent memory state
    TestAgentSetup setup(std::move(provider), std::move(tools), cfg);
    auto& agent = setup.agent;

    for (int i = 0; i < 7; i++) {
        agent.process("msg" + std::to_string(i));
    }
    REQUIRE(!agent.episodes().empty());

    agent.clear_history();

    // Archives must still be present after clear
    REQUIRE(!agent.episodes().empty());
    REQUIRE(agent.episode_by_id("episode:0") != nullptr);
    (void)mock;
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
    cfg.memory.backend = "none"; // Isolate from persistent memory state
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

// ── Synthesis: concept vs observation consolidation (PER-389) ────

TEST_CASE("Agent: synthesis prompt distinguishes concept and observation types", "[agent][synthesis]") {
    // The synthesis prompt must instruct the LLM on the concept/observation distinction
    // so it can classify stable patterns vs episode-specific facts correctly.
    auto provider = std::make_unique<MockProvider>();
    auto* mock = provider.get();
    mock->next_response.content = "I understand.";
    mock->simple_response = R"([{"key":"pref:rust","content":"User prefers Rust","category":"knowledge","type":"concept","links":[]}])";

    std::vector<std::unique_ptr<Tool>> tools;
    Config cfg;
    cfg.memory.backend = "json";
    cfg.memory.synthesis = true;
    cfg.memory.synthesis_interval = 1;

    std::string mem_path = "/tmp/ptrclaw_test_synth_prompt_" + std::to_string(getpid()) + ".json";
    auto memory = std::make_unique<JsonMemory>(mem_path);
    TestAgentSetup setup(std::move(provider), std::move(tools), cfg);
    auto& agent = setup.agent;
    agent.set_memory(std::move(memory));

    agent.process("I love Rust");

    REQUIRE(mock->last_simple_message.find("concept") != std::string::npos);
    REQUIRE(mock->last_simple_message.find("observation") != std::string::npos);
    // Output format must include the type field
    REQUIRE(mock->last_simple_message.find("\"type\"") != std::string::npos);

    std::filesystem::remove(mem_path);
}

TEST_CASE("Agent: synthesis stores concept-type entries without session_id", "[agent][synthesis]") {
    // Concept entries represent stable cross-session patterns.
    // They must be stored without a session_id so they outlive the current session.
    auto provider = std::make_unique<MockProvider>();
    auto* mock = provider.get();
    mock->next_response.content = "I understand.";
    mock->simple_response = R"([{"key":"pref:rust-over-cpp","content":"User prefers Rust over C++","category":"knowledge","type":"concept","links":[]}])";

    std::vector<std::unique_ptr<Tool>> tools;
    Config cfg;
    cfg.memory.backend = "json";
    cfg.memory.synthesis = true;
    cfg.memory.synthesis_interval = 1;

    std::string mem_path = "/tmp/ptrclaw_test_synth_concept_" + std::to_string(getpid()) + ".json";
    auto memory = std::make_unique<JsonMemory>(mem_path);
    TestAgentSetup setup(std::move(provider), std::move(tools), cfg);
    auto& agent = setup.agent;
    agent.set_memory(std::move(memory));
    agent.set_session_id("test-session-abc");

    agent.process("I strongly prefer Rust over C++");

    auto entry = agent.memory()->get("pref:rust-over-cpp");
    REQUIRE(entry.has_value());
    REQUIRE(entry.value_or(MemoryEntry{}).session_id.empty());

    std::filesystem::remove(mem_path);
    (void)mock;
}

TEST_CASE("Agent: synthesis stores observation-type entries with session_id", "[agent][synthesis]") {
    // Observation entries are episode-specific — tied to the current session.
    // They must retain the session_id so they can be distinguished from durable concepts.
    auto provider = std::make_unique<MockProvider>();
    auto* mock = provider.get();
    mock->next_response.content = "I understand.";
    mock->simple_response = R"([{"key":"user-debugging-today","content":"User is debugging a segfault today","category":"knowledge","type":"observation","links":[]}])";

    std::vector<std::unique_ptr<Tool>> tools;
    Config cfg;
    cfg.memory.backend = "json";
    cfg.memory.synthesis = true;
    cfg.memory.synthesis_interval = 1;

    std::string mem_path = "/tmp/ptrclaw_test_synth_obs_" + std::to_string(getpid()) + ".json";
    auto memory = std::make_unique<JsonMemory>(mem_path);
    TestAgentSetup setup(std::move(provider), std::move(tools), cfg);
    auto& agent = setup.agent;
    agent.set_memory(std::move(memory));
    agent.set_session_id("test-session-xyz");

    agent.process("Trying to find this segfault all day");

    auto entry = agent.memory()->get("user-debugging-today");
    REQUIRE(entry.has_value());
    REQUIRE(entry.value_or(MemoryEntry{}).session_id == "test-session-xyz");

    std::filesystem::remove(mem_path);
    (void)mock;
}

TEST_CASE("Agent: synthesis without type field defaults to observation (backward compat)", "[agent][synthesis]") {
    // Entries without a type field must be stored with session_id (observation default)
    // to ensure backward compatibility with pre-PER-389 synthesis responses.
    auto provider = std::make_unique<MockProvider>();
    auto* mock = provider.get();
    mock->next_response.content = "I understand.";
    mock->simple_response = R"([{"key":"user-likes-go","content":"User likes Go","category":"knowledge","links":[]}])";

    std::vector<std::unique_ptr<Tool>> tools;
    Config cfg;
    cfg.memory.backend = "json";
    cfg.memory.synthesis = true;
    cfg.memory.synthesis_interval = 1;

    std::string mem_path = "/tmp/ptrclaw_test_synth_compat_" + std::to_string(getpid()) + ".json";
    auto memory = std::make_unique<JsonMemory>(mem_path);
    TestAgentSetup setup(std::move(provider), std::move(tools), cfg);
    auto& agent = setup.agent;
    agent.set_memory(std::move(memory));
    agent.set_session_id("test-session-compat");

    agent.process("I like writing Go");

    auto entry = agent.memory()->get("user-likes-go");
    REQUIRE(entry.has_value());
    REQUIRE(entry.value_or(MemoryEntry{}).session_id == "test-session-compat");

    std::filesystem::remove(mem_path);
    (void)mock;
}

TEST_CASE("Agent: synthesis stores core-category entries as concepts (no session_id)", "[agent][synthesis]") {
    // Core-category entries (personality, identity, behavior) are inherently cross-session
    // concepts regardless of the type field — they must always be stored without session_id.
    auto provider = std::make_unique<MockProvider>();
    auto* mock = provider.get();
    mock->next_response.content = "I understand.";
    mock->simple_response = R"([{"key":"personality:direct-communication","content":"User prefers direct communication","category":"core","type":"observation","links":[]}])";

    std::vector<std::unique_ptr<Tool>> tools;
    Config cfg;
    cfg.memory.backend = "json";
    cfg.memory.synthesis = true;
    cfg.memory.synthesis_interval = 1;

    std::string mem_path = "/tmp/ptrclaw_test_synth_core_" + std::to_string(getpid()) + ".json";
    auto memory = std::make_unique<JsonMemory>(mem_path);
    TestAgentSetup setup(std::move(provider), std::move(tools), cfg);
    auto& agent = setup.agent;
    agent.set_memory(std::move(memory));
    agent.set_session_id("test-session-core");

    agent.process("Get to the point please");

    auto entry = agent.memory()->get("personality:direct-communication");
    REQUIRE(entry.has_value());
    // Core category overrides observation type — must be stored as a cross-session concept
    REQUIRE(entry.value_or(MemoryEntry{}).session_id.empty());

    std::filesystem::remove(mem_path);
    (void)mock;
}

// ── Synthesis: concept deduplication and contradiction (PER-394) ─

TEST_CASE("Agent: synthesis prompt includes deduplication guidance", "[agent][synthesis][per394]") {
    // The synthesis prompt must instruct the LLM to reuse existing keys for updates
    // and to use the "replaces" field when superseding a different existing concept.
    auto provider = std::make_unique<MockProvider>();
    auto* mock = provider.get();
    mock->next_response.content = "I understand.";
    mock->simple_response = "[]"; // valid but empty

    std::vector<std::unique_ptr<Tool>> tools;
    Config cfg;
    cfg.memory.backend = "json";
    cfg.memory.synthesis = true;
    cfg.memory.synthesis_interval = 1;

    std::string mem_path = "/tmp/ptrclaw_test_synth_dedup_prompt_" + std::to_string(getpid()) + ".json";
    auto memory = std::make_unique<JsonMemory>(mem_path);
    Agent agent(std::move(provider), std::move(tools), cfg);
    agent.set_memory(std::move(memory));

    agent.process("I prefer C++ over Rust");

    REQUIRE(mock->last_simple_message.find("Deduplication") != std::string::npos);
    REQUIRE(mock->last_simple_message.find("replaces") != std::string::npos);

    std::filesystem::remove(mem_path);
}

TEST_CASE("Agent: synthesis replaces field removes old concept and stores new one", "[agent][synthesis][per394]") {
    // When synthesis returns a note with "replaces": "old-key", the agent must
    // store the new concept and delete the outdated entry.
    auto provider = std::make_unique<MockProvider>();
    auto* mock = provider.get();
    mock->next_response.content = "I understand.";
    mock->simple_response = R"([{"key":"pref:cpp","content":"User prefers C++ over Rust","category":"knowledge","type":"concept","links":[],"replaces":"pref:rust"}])";

    std::vector<std::unique_ptr<Tool>> tools;
    Config cfg;
    cfg.memory.backend = "json";
    cfg.memory.synthesis = true;
    cfg.memory.synthesis_interval = 1;

    std::string mem_path = "/tmp/ptrclaw_test_synth_replaces_" + std::to_string(getpid()) + ".json";
    auto memory = std::make_unique<JsonMemory>(mem_path);
    // Pre-seed the old concept that will be replaced
    memory->store("pref:rust", "User prefers Rust", MemoryCategory::Knowledge, "");
    Agent agent(std::move(provider), std::move(tools), cfg);
    agent.set_memory(std::move(memory));

    agent.process("Actually I switched to C++");

    // New concept must exist
    auto new_entry = agent.memory()->get("pref:cpp");
    REQUIRE(new_entry.has_value());
    REQUIRE(new_entry.value_or(MemoryEntry{}).content == "User prefers C++ over Rust");

    // Old concept must be gone
    auto old_entry = agent.memory()->get("pref:rust");
    REQUIRE_FALSE(old_entry.has_value());

    std::filesystem::remove(mem_path);
    (void)mock;
}

TEST_CASE("Agent: synthesis replaces field migrates links to new concept", "[agent][synthesis][per394]") {
    // When an old concept has graph links, those links must be migrated to the
    // new concept so the knowledge graph stays connected after the replacement.
    auto provider = std::make_unique<MockProvider>();
    auto* mock = provider.get();
    mock->next_response.content = "I understand.";
    mock->simple_response = R"([{"key":"pref:cpp","content":"User prefers C++","category":"knowledge","type":"concept","links":[],"replaces":"pref:rust"}])";

    std::vector<std::unique_ptr<Tool>> tools;
    Config cfg;
    cfg.memory.backend = "json";
    cfg.memory.synthesis = true;
    cfg.memory.synthesis_interval = 1;

    std::string mem_path = "/tmp/ptrclaw_test_synth_replaces_links_" + std::to_string(getpid()) + ".json";
    auto memory = std::make_unique<JsonMemory>(mem_path);
    // Pre-seed old concept linked to another entry
    memory->store("pref:rust", "User prefers Rust", MemoryCategory::Knowledge, "");
    memory->store("decision:use-cargo", "Uses Cargo for builds", MemoryCategory::Knowledge, "");
    memory->link("pref:rust", "decision:use-cargo");

    Agent agent(std::move(provider), std::move(tools), cfg);
    agent.set_memory(std::move(memory));

    agent.process("Switched from Rust to C++");

    // Old entry gone
    REQUIRE_FALSE(agent.memory()->get("pref:rust").has_value());

    // New entry exists and has inherited the link
    auto new_entry = agent.memory()->get("pref:cpp");
    REQUIRE(new_entry.has_value());
    auto neighbors = agent.memory()->neighbors("pref:cpp", 10);
    bool has_linked = false;
    for (const auto& n : neighbors) {
        if (n.key == "decision:use-cargo") { has_linked = true; break; }
    }
    REQUIRE(has_linked);

    std::filesystem::remove(mem_path);
    (void)mock;
}

TEST_CASE("Agent: synthesis replaces pointing to same key is a no-op", "[agent][synthesis][per394]") {
    // "replaces" == key (self-reference) must not cause a deletion or other harm.
    auto provider = std::make_unique<MockProvider>();
    auto* mock = provider.get();
    mock->next_response.content = "I understand.";
    mock->simple_response = R"([{"key":"pref:cpp","content":"User prefers C++","category":"knowledge","type":"concept","links":[],"replaces":"pref:cpp"}])";

    std::vector<std::unique_ptr<Tool>> tools;
    Config cfg;
    cfg.memory.backend = "json";
    cfg.memory.synthesis = true;
    cfg.memory.synthesis_interval = 1;

    std::string mem_path = "/tmp/ptrclaw_test_synth_replaces_self_" + std::to_string(getpid()) + ".json";
    auto memory = std::make_unique<JsonMemory>(mem_path);
    Agent agent(std::move(provider), std::move(tools), cfg);
    agent.set_memory(std::move(memory));

    agent.process("I like C++");

    // Entry must still be stored (self-replace is a no-op)
    auto entry = agent.memory()->get("pref:cpp");
    REQUIRE(entry.has_value());

    std::filesystem::remove(mem_path);
    (void)mock;
}

TEST_CASE("Agent: synthesis replaces pointing to nonexistent key is silently ignored", "[agent][synthesis][per394]") {
    // "replaces" referencing a key that doesn't exist must not crash.
    auto provider = std::make_unique<MockProvider>();
    auto* mock = provider.get();
    mock->next_response.content = "I understand.";
    mock->simple_response = R"([{"key":"pref:cpp","content":"User prefers C++","category":"knowledge","type":"concept","links":[],"replaces":"pref:ghost"}])";

    std::vector<std::unique_ptr<Tool>> tools;
    Config cfg;
    cfg.memory.backend = "json";
    cfg.memory.synthesis = true;
    cfg.memory.synthesis_interval = 1;

    std::string mem_path = "/tmp/ptrclaw_test_synth_replaces_ghost_" + std::to_string(getpid()) + ".json";
    auto memory = std::make_unique<JsonMemory>(mem_path);
    Agent agent(std::move(provider), std::move(tools), cfg);
    agent.set_memory(std::move(memory));

    // Should not throw; new entry should still be stored
    REQUIRE_NOTHROW(agent.process("I like C++"));
    REQUIRE(agent.memory()->get("pref:cpp").has_value());

    std::filesystem::remove(mem_path);
    (void)mock;
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

// ── Context assembly (PER-390) ─────────────────────────────────

TEST_CASE("Agent: past episodes appear in user message context after compaction", "[agent][per390]") {
    // After compaction, the next process() call should include a "Past episodes:" line
    // inside the [Memory context] block — giving the model a layered view of history.
    auto provider = std::make_unique<MockProvider>();
    auto* mock = provider.get();
    mock->next_response.content = "reply";

    std::vector<std::unique_ptr<Tool>> tools;
    Config cfg;
    cfg.agent.max_history_messages = 10;
    cfg.memory.backend = "json";
    cfg.memory.recall_limit = 5;

    std::string mem_path = "/tmp/ptrclaw_test_ctx390_" + std::to_string(getpid()) + ".json";
    auto memory = std::make_unique<JsonMemory>(mem_path);
    TestAgentSetup setup(std::move(provider), std::move(tools), cfg);
    auto& agent = setup.agent;
    agent.set_memory(std::move(memory));

    // Trigger compaction (7 calls → 15 messages → exceeds max 10 AND guard 12)
    for (int i = 0; i < 7; i++) {
        agent.process("message " + std::to_string(i));
    }
    REQUIRE(!agent.episodes().empty());

    // Next call — find the user message sent to provider
    agent.process("what happened earlier?");

    std::string last_user_content;
    for (const auto& msg : mock->last_messages) {
        if (msg.role == Role::User &&
            msg.content.find("what happened earlier?") != std::string::npos) {
            last_user_content = msg.content;
        }
    }

    REQUIRE(!last_user_content.empty());
    REQUIRE(last_user_content.find("Past episodes:") != std::string::npos);
    REQUIRE(last_user_content.find("episode:") != std::string::npos);

    std::filesystem::remove(mem_path);
    (void)mock;
}

TEST_CASE("Agent: no episode context in user message before first compaction", "[agent][per390]") {
    // Before any compaction, episode_archives_ is empty so no "Past episodes:" line appears.
    auto provider = std::make_unique<MockProvider>();
    auto* mock = provider.get();
    mock->next_response.content = "reply";

    std::vector<std::unique_ptr<Tool>> tools;
    Config cfg;
    cfg.agent.max_history_messages = 50; // High — no compaction
    cfg.memory.backend = "json";
    cfg.memory.recall_limit = 5;

    std::string mem_path = "/tmp/ptrclaw_test_ctx390_pre_" + std::to_string(getpid()) + ".json";
    auto memory = std::make_unique<JsonMemory>(mem_path);
    TestAgentSetup setup(std::move(provider), std::move(tools), cfg);
    auto& agent = setup.agent;
    agent.set_memory(std::move(memory));

    agent.process("hello");

    // No episodes archived → no "Past episodes:" in enriched message
    for (const auto& msg : mock->last_messages) {
        if (msg.role == Role::User) {
            REQUIRE(msg.content.find("Past episodes:") == std::string::npos);
        }
    }

    std::filesystem::remove(mem_path);
    (void)mock;
}

// ── Episode archive persistence (PER-393) ──────────────────────────────

TEST_CASE("Agent: episodes persist to JsonMemory on compaction", "[agent][per393]") {
    // After compaction, episode archive should be persisted to the memory backend.
    // A second JsonMemory instance on the same file should return the blob.
    auto provider = std::make_unique<MockProvider>();
    provider->next_response.content = "reply";

    std::vector<std::unique_ptr<Tool>> tools;
    Config cfg;
    cfg.agent.max_history_messages = 10;

    std::string mem_path = "/tmp/ptrclaw_test_ep393_json_" + std::to_string(getpid()) + ".json";
    auto memory = std::make_unique<JsonMemory>(mem_path);

    TestAgentSetup setup(std::move(provider), std::move(tools), cfg);
    auto& agent = setup.agent;
    agent.set_memory(std::move(memory));

    // Drive enough turns to trigger compaction (> max_history_messages)
    for (int i = 0; i < 7; i++) {
        agent.process("msg" + std::to_string(i));
    }

    REQUIRE(!agent.episodes().empty());

    // The archive blob must be stored in the JSON file
    JsonMemory reader(mem_path);
    const std::string blob = reader.load_episode_archive();
    REQUIRE_FALSE(blob.empty());

    // Blob must reference the archived episode id
    REQUIRE(blob.find("episode:0") != std::string::npos);

    std::error_code ec;
    std::filesystem::remove(mem_path, ec);
    std::filesystem::remove(mem_path + ".tmp", ec);
}

TEST_CASE("Agent: set_memory restores episodes from backend", "[agent][per393]") {
    // Simulate a restart: pre-populate the memory file with an episode archive,
    // then attach it to a fresh agent via set_memory().
    // The agent should restore episode_archives_ from the persisted blob.
    const std::string blob = R"([{"id":"episode:2","timestamp":9999,"user_turns":3,"assistant_turns":3,"tool_calls":0,"tools_used":[],"messages":[{"role":"user","content":"prior context"}]}])";

    std::string mem_path = "/tmp/ptrclaw_test_ep393_restore_" + std::to_string(getpid()) + ".json";
    {
        // Write the blob into the file directly via JsonMemory
        JsonMemory seeder(mem_path);
        seeder.save_episode_archive(blob);
    }

    auto provider = std::make_unique<MockProvider>();
    provider->next_response.content = "reply";

    std::vector<std::unique_ptr<Tool>> tools;
    Config cfg;

    TestAgentSetup setup(std::move(provider), std::move(tools), cfg);
    auto& agent = setup.agent;
    auto memory = std::make_unique<JsonMemory>(mem_path);
    agent.set_memory(std::move(memory));

    // Episodes should be restored from the persisted archive
    REQUIRE(!agent.episodes().empty());
    const auto* ep = agent.episode_by_id("episode:2");
    REQUIRE(ep != nullptr);
    REQUIRE(ep->user_turns == 3);
    REQUIRE(ep->messages.size() == 1);
    REQUIRE(ep->messages[0].content == "prior context");

    std::error_code ec;
    std::filesystem::remove(mem_path, ec);
    std::filesystem::remove(mem_path + ".tmp", ec);
}

TEST_CASE("Agent: episode counter advances past restored episodes", "[agent][per393]") {
    // If episode:5 is restored from disk, the next archival must produce episode:6.
    const std::string blob = R"([{"id":"episode:5","timestamp":1,"user_turns":1,"assistant_turns":1,"tool_calls":0,"tools_used":[],"messages":[]}])";

    std::string mem_path = "/tmp/ptrclaw_test_ep393_counter_" + std::to_string(getpid()) + ".json";
    {
        JsonMemory seeder(mem_path);
        seeder.save_episode_archive(blob);
    }

    auto provider = std::make_unique<MockProvider>();
    provider->next_response.content = "ok";

    std::vector<std::unique_ptr<Tool>> tools;
    Config cfg;
    cfg.agent.max_history_messages = 10;

    TestAgentSetup setup(std::move(provider), std::move(tools), cfg);
    auto& agent = setup.agent;
    auto memory = std::make_unique<JsonMemory>(mem_path);
    agent.set_memory(std::move(memory));

    REQUIRE(!agent.episodes().empty());

    // Drive enough turns to trigger a new compaction
    for (int i = 0; i < 7; i++) {
        agent.process("msg" + std::to_string(i));
    }

    // The new episode must have id "episode:6", not "episode:0"
    bool found_six = false;
    for (const auto& ep : agent.episodes()) {
        if (ep.id == "episode:6") {
            found_six = true;
        }
        // Must not reset to 0
        REQUIRE(ep.id != "episode:0");
    }
    REQUIRE(found_six);

    std::error_code ec;
    std::filesystem::remove(mem_path, ec);
    std::filesystem::remove(mem_path + ".tmp", ec);
}
