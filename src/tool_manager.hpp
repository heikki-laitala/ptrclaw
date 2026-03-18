#pragma once
#include "tool.hpp"
#include "event.hpp"
#include "event_bus.hpp"
#include "config.hpp"
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <cstdint>
#include <future>
#include <unordered_map>

namespace ptrclaw {

class Memory; // forward declaration

// Collects ToolCallResultEvents for a batch, allowing the caller to block
// until all expected results arrive.
class BatchCollector {
public:
    BatchCollector(const std::string& batch_id, size_t expected_count);

    // Called by EventBus handler when a ToolCallResultEvent arrives.
    void on_result(const ToolCallResultEvent& ev);

    // Block until all expected results arrive. Returns true if all
    // results arrived, false if the timeout expired first.
    // A zero timeout means wait indefinitely.
    bool wait(std::chrono::seconds timeout = std::chrono::seconds{0});

    // Retrieve collected results (valid after wait() returns).
    // On timeout, contains only the results received so far.
    std::vector<ToolCallResultEvent> results() const;

    // Number of results still missing after wait() returns.
    size_t missing() const;

private:
    std::string batch_id_;
    size_t expected_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<ToolCallResultEvent> results_;
};

// Event-driven tool executor. Subscribes to ToolCallRequestEvents on the bus,
// executes tools (with output filtering), and publishes ToolCallResultEvents.
// Publishes ToolsAvailableEvent when the set of available tools changes.
class ToolManager {
public:
    ToolManager(std::vector<std::unique_ptr<Tool>> tools,
                const Config& config,
                EventBus& bus,
                const std::string& session_id = "");
    ~ToolManager();

    // Publish current tool specs as ToolsAvailableEvent.
    void publish_tool_specs(const std::string& session_id = "");

    // Tool wiring (delegates to individual tools)
    void wire_memory(Memory* memory);
    void reset_all();

    // Access tools (e.g. for system prompt building by non-native providers)
    const std::vector<std::unique_ptr<Tool>>& tools() const { return tools_; }

private:
    void on_tool_call_request(const ToolCallRequestEvent& ev);
    void on_tool_call_cancel(const ToolCallCancelEvent& ev);
    void execute_and_publish(const ToolCallRequestEvent& ev,
                             const CancellationToken& token);
    ToolResult execute_tool(const std::string& name, const std::string& args_json,
                            const CancellationToken& token);
    std::string apply_filters(const std::string& tool_name,
                              const std::string& args_json,
                              bool success,
                              std::string output);
    void cleanup_futures();

    std::vector<std::unique_ptr<Tool>> tools_;
    Config config_;
    EventBus& bus_;
    std::string session_id_;  // empty = accept all sessions (CLI mode)
    uint64_t request_sub_id_ = 0;
    uint64_t cancel_sub_id_ = 0;
    bool memory_active_ = false;
    std::vector<std::future<void>> pending_futures_;
    std::mutex futures_mutex_;
    struct ActiveCall {
        CancellationToken token;
        std::string batch_id;
    };
    std::unordered_map<std::string, ActiveCall> active_calls_;  // keyed by tool_call_id
    std::mutex calls_mutex_;
};

} // namespace ptrclaw
