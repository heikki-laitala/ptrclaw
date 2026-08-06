#include <catch2/catch_test_macros.hpp>
#include "tool_manager.hpp"
#include "event_bus.hpp"
#include "memory.hpp"
#include <chrono>
#include <thread>
#include <atomic>

using namespace ptrclaw;

// ── Mock tools ──────────────────────────────────────────────────

class MockTool : public Tool {
public:
    explicit MockTool(std::string name = "mock_tool")
        : name_(std::move(name)) {}
    ToolResult execute(const std::string&) override {
        return {true, "result"};
    }
    std::string tool_name() const override { return name_; }
    std::string description() const override { return "mock tool"; }
    std::string parameters_json() const override { return R"({"type":"object"})"; }
private:
    std::string name_;
};

class SlowMockTool : public Tool {
public:
    // Shared counter tracks concurrent executions
    explicit SlowMockTool(std::string name,
                          std::atomic<int>& concurrency,
                          std::atomic<int>& max_concurrency)
        : name_(std::move(name)), concurrency_(concurrency),
          max_concurrency_(max_concurrency) {}

    ToolResult execute(const std::string&) override {
        int cur = ++concurrency_;
        // Update peak concurrency
        int prev = max_concurrency_.load();
        while (cur > prev && !max_concurrency_.compare_exchange_weak(prev, cur)) {}
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        --concurrency_;
        return {true, "slow-result"};
    }
    std::string tool_name() const override { return name_; }
    std::string description() const override { return "slow tool"; }
    std::string parameters_json() const override { return R"({"type":"object"})"; }
private:
    std::string name_;
    std::atomic<int>& concurrency_;
    std::atomic<int>& max_concurrency_;
};

class CancellableMockTool : public Tool {
public:
    std::atomic<bool> was_cancelled{false};

    ToolResult execute(const std::string&) override {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        return {true, "should-not-reach"};
    }
    ToolResult execute(const std::string&, const CancellationToken& token) override {
        for (int i = 0; i < 50; ++i) {
            if (is_cancelled(token)) {
                was_cancelled.store(true);
                return {false, "cancelled"};
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return {true, "completed"};
    }
    std::string tool_name() const override { return "cancellable"; }
    std::string description() const override { return "cancellable tool"; }
    std::string parameters_json() const override { return R"({"type":"object"})"; }
};

// Helper to build a minimal Config
static Config make_config() {
    Config c;
    c.agent.tee_mode = "off";
    return c;
}

// ── Tests ───────────────────────────────────────────────────────

TEST_CASE("ToolManager: executes tool and publishes result", "[tool_manager]") {
    EventBus bus;
    std::vector<std::unique_ptr<Tool>> tools;
    tools.push_back(std::make_unique<MockTool>());
    ToolManager mgr(std::move(tools), make_config(), bus);

    BatchCollector collector("batch-1", 1);
    auto sub = subscribe<ToolCallResultEvent>(bus,
        std::function<void(const ToolCallResultEvent&)>(
            [&collector](const ToolCallResultEvent& ev) {
                collector.on_result(ev);
            }));

    ToolCallRequestEvent req;
    req.batch_id = "batch-1";
    req.tool_name = "mock_tool";
    req.tool_call_id = "call-1";
    req.arguments_json = "{}";
    bus.publish(req);

    collector.wait();
    bus.unsubscribe(sub);

    auto results = collector.results();
    REQUIRE(results.size() == 1);
    REQUIRE(results[0].success);
    REQUIRE(results[0].tool_name == "mock_tool");
}

TEST_CASE("ToolManager: multiple tools run concurrently", "[tool_manager]") {
    EventBus bus;

    std::atomic<int> concurrency{0};
    std::atomic<int> max_concurrency{0};

    std::vector<std::unique_ptr<Tool>> tools;
    tools.push_back(std::make_unique<SlowMockTool>("slow_a", concurrency, max_concurrency));
    tools.push_back(std::make_unique<SlowMockTool>("slow_b", concurrency, max_concurrency));
    ToolManager mgr(std::move(tools), make_config(), bus);

    BatchCollector collector("batch-2", 2);
    auto sub = subscribe<ToolCallResultEvent>(bus,
        std::function<void(const ToolCallResultEvent&)>(
            [&collector](const ToolCallResultEvent& ev) {
                collector.on_result(ev);
            }));

    ToolCallRequestEvent req1;
    req1.batch_id = "batch-2";
    req1.tool_name = "slow_a";
    req1.tool_call_id = "call-a";
    req1.arguments_json = "{}";

    ToolCallRequestEvent req2;
    req2.batch_id = "batch-2";
    req2.tool_name = "slow_b";
    req2.tool_call_id = "call-b";
    req2.arguments_json = "{}";

    bus.publish(req1);
    bus.publish(req2);

    collector.wait();
    bus.unsubscribe(sub);

    auto results = collector.results();
    REQUIRE(results.size() == 2);

    // Both tools should have been executing at the same time
    REQUIRE(max_concurrency.load() == 2);
}

TEST_CASE("ToolManager: unknown tool returns error", "[tool_manager]") {
    EventBus bus;
    std::vector<std::unique_ptr<Tool>> tools;
    ToolManager mgr(std::move(tools), make_config(), bus);

    BatchCollector collector("batch-3", 1);
    auto sub = subscribe<ToolCallResultEvent>(bus,
        std::function<void(const ToolCallResultEvent&)>(
            [&collector](const ToolCallResultEvent& ev) {
                collector.on_result(ev);
            }));

    ToolCallRequestEvent req;
    req.batch_id = "batch-3";
    req.tool_name = "nonexistent";
    req.tool_call_id = "call-x";
    req.arguments_json = "{}";
    bus.publish(req);

    collector.wait();
    bus.unsubscribe(sub);

    auto results = collector.results();
    REQUIRE(results.size() == 1);
    REQUIRE_FALSE(results[0].success);
    REQUIRE(results[0].output.find("Unknown tool") != std::string::npos);
}

TEST_CASE("ToolManager: publish_tool_specs emits ToolsAvailableEvent", "[tool_manager]") {
    EventBus bus;

    std::vector<std::unique_ptr<Tool>> tools;
    tools.push_back(std::make_unique<MockTool>());

    ToolManager mgr(std::move(tools), make_config(), bus);

    std::vector<ToolSpec> received_specs;
    auto sub = subscribe<ToolsAvailableEvent>(bus,
        std::function<void(const ToolsAvailableEvent&)>(
            [&received_specs](const ToolsAvailableEvent& ev) {
                received_specs = ev.specs;
            }));

    mgr.publish_tool_specs("test-session");
    bus.unsubscribe(sub);

    REQUIRE(received_specs.size() == 1);
    REQUIRE(received_specs[0].name == "mock_tool");
}

TEST_CASE("ToolManager: result preserves batch_id and tool_call_id", "[tool_manager]") {
    EventBus bus;
    std::vector<std::unique_ptr<Tool>> tools;
    tools.push_back(std::make_unique<MockTool>());
    ToolManager mgr(std::move(tools), make_config(), bus);

    BatchCollector collector("my-batch", 1);
    auto sub = subscribe<ToolCallResultEvent>(bus,
        std::function<void(const ToolCallResultEvent&)>(
            [&collector](const ToolCallResultEvent& ev) {
                collector.on_result(ev);
            }));

    ToolCallRequestEvent req;
    req.session_id = "sess-42";
    req.batch_id = "my-batch";
    req.tool_name = "mock_tool";
    req.tool_call_id = "tc-99";
    req.arguments_json = "{}";
    bus.publish(req);

    collector.wait();
    bus.unsubscribe(sub);

    auto results = collector.results();
    REQUIRE(results[0].batch_id == "my-batch");
    REQUIRE(results[0].tool_call_id == "tc-99");
    REQUIRE(results[0].session_id == "sess-42");
}

TEST_CASE("BatchCollector: wait returns false on timeout", "[tool_manager]") {
    BatchCollector collector("timeout-batch", 1);
    // No results will arrive — should timeout
    bool completed = collector.wait(std::chrono::seconds{1});
    REQUIRE_FALSE(completed);
    REQUIRE(collector.missing() == 1);
    REQUIRE(collector.results().empty());
}

TEST_CASE("BatchCollector: wait returns true when results arrive before timeout", "[tool_manager]") {
    BatchCollector collector("fast-batch", 1);

    ToolCallResultEvent ev;
    ev.batch_id = "fast-batch";
    ev.tool_call_id = "call-1";
    ev.tool_name = "test";
    ev.success = true;
    ev.output = "ok";
    collector.on_result(ev);

    bool completed = collector.wait(std::chrono::seconds{1});
    REQUIRE(completed);
    REQUIRE(collector.missing() == 0);
    REQUIRE(collector.results().size() == 1);
}

TEST_CASE("ToolManager: ToolCallCancelEvent cancels running tools", "[tool_manager]") {
    EventBus bus;

    auto* raw_ptr = new CancellableMockTool();
    std::vector<std::unique_ptr<Tool>> tools;
    tools.push_back(std::unique_ptr<Tool>(raw_ptr));
    ToolManager mgr(std::move(tools), make_config(), bus);

    BatchCollector collector("cancel-batch", 1);
    auto sub = subscribe<ToolCallResultEvent>(bus,
        std::function<void(const ToolCallResultEvent&)>(
            [&collector](const ToolCallResultEvent& ev) {
                collector.on_result(ev);
            }));

    ToolCallRequestEvent req;
    req.batch_id = "cancel-batch";
    req.tool_name = "cancellable";
    req.tool_call_id = "call-cancel";
    req.arguments_json = "{}";
    bus.publish(req);

    // Give the tool time to start executing
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    // Publish cancel event
    ToolCallCancelEvent cancel_ev;
    cancel_ev.batch_id = "cancel-batch";
    bus.publish(cancel_ev);

    bool completed = collector.wait(std::chrono::seconds{2});
    bus.unsubscribe(sub);

    REQUIRE(completed);
    auto results = collector.results();
    REQUIRE(results.size() == 1);
    REQUIRE_FALSE(results[0].success);
    REQUIRE(raw_ptr->was_cancelled.load());
}

TEST_CASE("CancellationToken: basic operations", "[tool_manager]") {
    auto token = make_cancellation_token();
    REQUIRE_FALSE(is_cancelled(token));
    cancel(token);
    REQUIRE(is_cancelled(token));
}

TEST_CASE("CancellationToken: null token is not cancelled", "[tool_manager]") {
    CancellationToken null_token;
    REQUIRE_FALSE(is_cancelled(null_token));
}

// ── workspace wiring ────────────────────────────────────────────

namespace {

// Reports what it was handed, which is the only thing under test here — the resolution
// itself is covered in tests/test_workspace.cpp.
class WorkspaceProbeTool : public WorkspaceAwareTool {
public:
    ToolResult execute(const std::string&) override {
        return {true, workspace_.workspace + "|" + workspace_.context_dir};
    }
    std::string tool_name() const override { return "workspace_probe"; }
    std::string description() const override { return "probe"; }
    std::string parameters_json() const override { return R"({"type":"object"})"; }
};

} // namespace

// The same injection point as EventBusAwareTool: a tool's execute() still receives nothing
// but its arguments, so the scope has to arrive at construction.
TEST_CASE("ToolManager: hands a scoped tool this session's workspace", "[tool_manager]") {
    EventBus bus;
    Config config;
    config.serving.workspace_root = "/work/sessions";
    config.serving.context_dir = "/work/context";

    std::vector<std::unique_ptr<Tool>> tools;
    tools.push_back(std::make_unique<WorkspaceProbeTool>());
    ToolManager manager(std::move(tools), config, bus, "task-42");

    auto result = manager.tools().front()->execute("{}");
    REQUIRE(result.success);
    REQUIRE(result.output.find("/work/sessions/") != std::string::npos);
    REQUIRE(result.output.find(session_store_key("task-42")) != std::string::npos);
    REQUIRE(result.output.find("|/work/context") != std::string::npos);
}

// Two sessions must not be handed the same directory, or the isolation is decorative.
TEST_CASE("ToolManager: different sessions get different workspaces", "[tool_manager]") {
    EventBus bus;
    Config config;
    config.serving.workspace_root = "/work/sessions";

    auto probe_for = [&](const std::string& session_id) {
        std::vector<std::unique_ptr<Tool>> tools;
        tools.push_back(std::make_unique<WorkspaceProbeTool>());
        ToolManager manager(std::move(tools), config, bus, session_id);
        return manager.tools().front()->execute("{}").output;
    };

    REQUIRE(probe_for("task-1") != probe_for("task-2"));
}

// With no serving config there is no scope, and the scoped tools then refuse everything
// rather than reaching the process cwd — which is the personal-agent shape.
TEST_CASE("ToolManager: no serving config means no workspace", "[tool_manager]") {
    EventBus bus;
    Config config;

    std::vector<std::unique_ptr<Tool>> tools;
    tools.push_back(std::make_unique<WorkspaceProbeTool>());
    ToolManager manager(std::move(tools), config, bus, "task-42");

    REQUIRE(manager.tools().front()->execute("{}").output == "|");
}
