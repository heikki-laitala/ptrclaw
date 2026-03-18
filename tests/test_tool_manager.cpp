#include <catch2/catch_test_macros.hpp>
#include "tool_manager.hpp"
#include "event_bus.hpp"
#include <atomic>
#include <chrono>
#include <thread>

using namespace ptrclaw;

// ── Mock tools ──────────────────────────────────────────────────

class SequentialMockTool : public Tool {
public:
    ToolResult execute(const std::string&) override {
        ++call_count;
        return {true, "seq-result"};
    }
    std::string tool_name() const override { return "seq_tool"; }
    std::string description() const override { return "sequential tool"; }
    std::string parameters_json() const override { return R"({"type":"object"})"; }
    bool is_parallel_safe() const override { return false; }

    std::atomic<int> call_count{0};
};

class ParallelMockTool : public Tool {
public:
    ToolResult execute(const std::string&) override {
        ++call_count;
        // Sleep briefly to make parallel execution observable
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        return {true, "par-result"};
    }
    std::string tool_name() const override { return "par_tool"; }
    std::string description() const override { return "parallel tool"; }
    std::string parameters_json() const override { return R"({"type":"object"})"; }
    bool is_parallel_safe() const override { return true; }

    std::atomic<int> call_count{0};
};

// Helper to build a minimal Config
static Config make_config() {
    Config c;
    c.agent.tee_mode = "off";
    return c;
}

// ── Tests ───────────────────────────────────────────────────────

TEST_CASE("ToolManager: sequential tool executes synchronously", "[tool_manager]") {
    EventBus bus;
    auto seq = std::make_unique<SequentialMockTool>();
    auto* seq_ptr = seq.get();

    std::vector<std::unique_ptr<Tool>> tools;
    tools.push_back(std::move(seq));
    ToolManager mgr(std::move(tools), make_config(), bus);

    BatchCollector collector("batch-1", 1);
    auto sub = subscribe<ToolCallResultEvent>(bus,
        std::function<void(const ToolCallResultEvent&)>(
            [&collector](const ToolCallResultEvent& ev) {
                collector.on_result(ev);
            }));

    ToolCallRequestEvent req;
    req.batch_id = "batch-1";
    req.tool_name = "seq_tool";
    req.tool_call_id = "call-1";
    req.arguments_json = "{}";
    bus.publish(req);

    collector.wait();
    bus.unsubscribe(sub);

    auto results = collector.results();
    REQUIRE(results.size() == 1);
    REQUIRE(results[0].success);
    REQUIRE(results[0].tool_name == "seq_tool");
    REQUIRE(seq_ptr->call_count == 1);
}

TEST_CASE("ToolManager: parallel tool executes via async", "[tool_manager]") {
    EventBus bus;
    auto par = std::make_unique<ParallelMockTool>();
    auto* par_ptr = par.get();

    std::vector<std::unique_ptr<Tool>> tools;
    tools.push_back(std::move(par));
    ToolManager mgr(std::move(tools), make_config(), bus);

    BatchCollector collector("batch-2", 1);
    auto sub = subscribe<ToolCallResultEvent>(bus,
        std::function<void(const ToolCallResultEvent&)>(
            [&collector](const ToolCallResultEvent& ev) {
                collector.on_result(ev);
            }));

    ToolCallRequestEvent req;
    req.batch_id = "batch-2";
    req.tool_name = "par_tool";
    req.tool_call_id = "call-1";
    req.arguments_json = "{}";
    bus.publish(req);

    collector.wait();
    bus.unsubscribe(sub);

    auto results = collector.results();
    REQUIRE(results.size() == 1);
    REQUIRE(results[0].success);
    REQUIRE(results[0].tool_name == "par_tool");
    REQUIRE(par_ptr->call_count == 1);
}

TEST_CASE("ToolManager: multiple parallel tools run concurrently", "[tool_manager]") {
    EventBus bus;
    auto par1 = std::make_unique<ParallelMockTool>();
    auto par2 = std::make_unique<ParallelMockTool>();

    // Both tools need different names for dispatch
    class NamedParallelTool : public Tool {
    public:
        explicit NamedParallelTool(std::string name) : name_(std::move(name)) {}
        ToolResult execute(const std::string&) override {
            auto start = std::chrono::steady_clock::now();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            auto end = std::chrono::steady_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
            return {true, std::to_string(ms)};
        }
        std::string tool_name() const override { return name_; }
        std::string description() const override { return "parallel"; }
        std::string parameters_json() const override { return R"({"type":"object"})"; }
        bool is_parallel_safe() const override { return true; }
    private:
        std::string name_;
    };

    std::vector<std::unique_ptr<Tool>> tools;
    tools.push_back(std::make_unique<NamedParallelTool>("par_a"));
    tools.push_back(std::make_unique<NamedParallelTool>("par_b"));
    ToolManager mgr(std::move(tools), make_config(), bus);

    BatchCollector collector("batch-3", 2);
    auto sub = subscribe<ToolCallResultEvent>(bus,
        std::function<void(const ToolCallResultEvent&)>(
            [&collector](const ToolCallResultEvent& ev) {
                collector.on_result(ev);
            }));

    auto start = std::chrono::steady_clock::now();

    ToolCallRequestEvent req1;
    req1.batch_id = "batch-3";
    req1.tool_name = "par_a";
    req1.tool_call_id = "call-a";
    req1.arguments_json = "{}";

    ToolCallRequestEvent req2;
    req2.batch_id = "batch-3";
    req2.tool_name = "par_b";
    req2.tool_call_id = "call-b";
    req2.arguments_json = "{}";

    bus.publish(req1);
    bus.publish(req2);

    collector.wait();
    bus.unsubscribe(sub);

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    auto results = collector.results();
    REQUIRE(results.size() == 2);

    // If truly parallel, total time should be ~100ms, not ~200ms
    // Allow generous margin for CI
    REQUIRE(elapsed < 180);
}

TEST_CASE("ToolManager: mixed parallel and sequential batch", "[tool_manager]") {
    EventBus bus;

    class SeqTool : public Tool {
    public:
        ToolResult execute(const std::string&) override { return {true, "seq"}; }
        std::string tool_name() const override { return "seq"; }
        std::string description() const override { return "seq"; }
        std::string parameters_json() const override { return R"({"type":"object"})"; }
        bool is_parallel_safe() const override { return false; }
    };

    class ParTool : public Tool {
    public:
        ToolResult execute(const std::string&) override {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            return {true, "par"};
        }
        std::string tool_name() const override { return "par"; }
        std::string description() const override { return "par"; }
        std::string parameters_json() const override { return R"({"type":"object"})"; }
        bool is_parallel_safe() const override { return true; }
    };

    std::vector<std::unique_ptr<Tool>> tools;
    tools.push_back(std::make_unique<SeqTool>());
    tools.push_back(std::make_unique<ParTool>());
    ToolManager mgr(std::move(tools), make_config(), bus);

    BatchCollector collector("batch-4", 2);
    auto sub = subscribe<ToolCallResultEvent>(bus,
        std::function<void(const ToolCallResultEvent&)>(
            [&collector](const ToolCallResultEvent& ev) {
                collector.on_result(ev);
            }));

    ToolCallRequestEvent req_seq;
    req_seq.batch_id = "batch-4";
    req_seq.tool_name = "seq";
    req_seq.tool_call_id = "call-seq";
    req_seq.arguments_json = "{}";

    ToolCallRequestEvent req_par;
    req_par.batch_id = "batch-4";
    req_par.tool_name = "par";
    req_par.tool_call_id = "call-par";
    req_par.arguments_json = "{}";

    bus.publish(req_seq);
    bus.publish(req_par);

    collector.wait();
    bus.unsubscribe(sub);

    auto results = collector.results();
    REQUIRE(results.size() == 2);

    // Verify both tools executed
    bool found_seq = false;
    bool found_par = false;
    for (const auto& r : results) {
        if (r.tool_name == "seq") found_seq = true;
        if (r.tool_name == "par") found_par = true;
    }
    REQUIRE(found_seq);
    REQUIRE(found_par);
}

TEST_CASE("ToolManager: unknown tool returns error", "[tool_manager]") {
    EventBus bus;
    std::vector<std::unique_ptr<Tool>> tools;
    ToolManager mgr(std::move(tools), make_config(), bus);

    BatchCollector collector("batch-5", 1);
    auto sub = subscribe<ToolCallResultEvent>(bus,
        std::function<void(const ToolCallResultEvent&)>(
            [&collector](const ToolCallResultEvent& ev) {
                collector.on_result(ev);
            }));

    ToolCallRequestEvent req;
    req.batch_id = "batch-5";
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
    tools.push_back(std::make_unique<SequentialMockTool>());

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
    REQUIRE(received_specs[0].name == "seq_tool");
}

TEST_CASE("ToolManager: result preserves batch_id and tool_call_id", "[tool_manager]") {
    EventBus bus;
    std::vector<std::unique_ptr<Tool>> tools;
    tools.push_back(std::make_unique<SequentialMockTool>());
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
    req.tool_name = "seq_tool";
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
