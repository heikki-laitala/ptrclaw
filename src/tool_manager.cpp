#include "tool_manager.hpp"
#include "memory.hpp"
#include "output_filter.hpp"
#include "util.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
#include <future>

namespace ptrclaw {

// ── BatchCollector ──────────────────────────────────────────────

BatchCollector::BatchCollector(const std::string& batch_id, size_t expected_count)
    : batch_id_(batch_id), expected_(expected_count) {}

void BatchCollector::on_result(const ToolCallResultEvent& ev) {
    if (ev.batch_id != batch_id_) return;
    std::lock_guard<std::mutex> lock(mutex_);
    results_.push_back(ev);
    if (results_.size() >= expected_) {
        cv_.notify_one();
    }
}

bool BatchCollector::wait(std::chrono::seconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (timeout.count() == 0) {
        cv_.wait(lock, [this] { return results_.size() >= expected_; });
        return true;
    }
    return cv_.wait_for(lock, timeout,
        [this] { return results_.size() >= expected_; });
}

std::vector<ToolCallResultEvent> BatchCollector::results() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return results_;
}

size_t BatchCollector::missing() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return expected_ > results_.size() ? expected_ - results_.size() : 0;
}

// ── ToolManager ─────────────────────────────────────────────────

ToolManager::ToolManager(std::vector<std::unique_ptr<Tool>> tools,
                         const Config& config,
                         EventBus& bus)
    : tools_(std::move(tools)), config_(config), bus_(bus) {
    subscription_id_ = subscribe<ToolCallRequestEvent>(bus_,
        std::function<void(const ToolCallRequestEvent&)>(
            [this](const ToolCallRequestEvent& ev) { on_tool_call_request(ev); }));

    // Wire EventBus into tools that need it
    for (auto& tool : tools_) {
        auto* ebt = dynamic_cast<EventBusAwareTool*>(tool.get());
        if (ebt) ebt->set_event_bus(&bus_);
    }
}

void ToolManager::publish_tool_specs(const std::string& session_id) {
    ToolsAvailableEvent ev;
    ev.session_id = session_id;
    for (const auto& tool : tools_) {
        if (is_memory_tool(tool->tool_name()) && !memory_active_) {
            continue;
        }
        ev.specs.push_back(tool->spec());
    }
    bus_.publish(ev);
}

void ToolManager::wire_memory(Memory* memory) {
    memory_active_ = (memory != nullptr && memory->backend_name() != "none");
    for (auto& tool : tools_) {
        if (is_memory_tool(tool->tool_name())) {
            auto* mat = dynamic_cast<MemoryAwareTool*>(tool.get());
            if (mat) {
                mat->set_memory(memory);
            }
        }
    }
}

void ToolManager::reset_all() {
    for (auto& tool : tools_) {
        tool->reset();
    }
}

ToolManager::~ToolManager() {
    std::lock_guard<std::mutex> lock(futures_mutex_);
    for (auto& f : pending_futures_) {
        if (f.valid()) f.wait();
    }
}

void ToolManager::on_tool_call_request(const ToolCallRequestEvent& ev) {
    std::cerr << "[tool] " << ev.tool_name << '\n';

    cleanup_futures();
    std::lock_guard<std::mutex> lock(futures_mutex_);
    pending_futures_.push_back(
        std::async(std::launch::async,
            [this, ev]() noexcept { // NOLINT(bugprone-exception-escape)
                try {
                    execute_and_publish(ev);
                } catch (...) {} // NOLINT(bugprone-empty-catch)
            }));
}

void ToolManager::execute_and_publish(const ToolCallRequestEvent& ev) {
    auto result = execute_tool(ev.tool_name, ev.arguments_json);

    uint32_t raw_tokens = estimate_tokens(result.output);
    result.output = apply_filters(ev.tool_name, ev.arguments_json,
                                   result.success, std::move(result.output));
    uint32_t filtered_tokens = estimate_tokens(result.output);

    if (raw_tokens > filtered_tokens + 10) {
        std::cerr << "[filter] " << ev.tool_name << ": "
                  << raw_tokens << " -> " << filtered_tokens << " tokens ("
                  << (raw_tokens - filtered_tokens) << " saved)\n";
    }

    ToolCallResultEvent rev;
    rev.session_id = ev.session_id;
    rev.batch_id = ev.batch_id;
    rev.tool_call_id = ev.tool_call_id;
    rev.tool_name = ev.tool_name;
    rev.success = result.success;
    rev.output = std::move(result.output);
    rev.raw_tokens = raw_tokens;
    rev.filtered_tokens = filtered_tokens;
    bus_.publish(rev);
}

ToolResult ToolManager::execute_tool(const std::string& name,
                                     const std::string& args_json) {
    for (const auto& tool : tools_) {
        if (tool->tool_name() == name) return tool->execute(args_json);
    }
    return ToolResult{false, "Unknown tool: " + name};
}

void ToolManager::cleanup_futures() {
    std::lock_guard<std::mutex> lock(futures_mutex_);
    pending_futures_.erase(
        std::remove_if(pending_futures_.begin(), pending_futures_.end(),
            [](const std::future<void>& f) {
                return f.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
            }),
        pending_futures_.end());
}

std::string ToolManager::apply_filters(const std::string& tool_name,
                                       const std::string& args_json,
                                       bool success,
                                       std::string output) {
    if (tool_name == "shell") {
        std::string command;
        try {
            auto args = nlohmann::json::parse(args_json);
            command = args.value("command", "");
        } catch (...) {} // NOLINT(bugprone-empty-catch)

        // Tee output to disk before filtering (opt-in via config)
        std::string tee_path;
        if (!command.empty() && config_.agent.tee_mode != "off") {
            bool should_tee = (config_.agent.tee_mode == "always") ||
                              (config_.agent.tee_mode == "failures" && !success);
            if (should_tee) {
                tee_path = tee_shell_output(command, output);
            }
        }

        output = deduplicate_log_lines(output);
        output = filter_shell_output(command, output);

        if (!tee_path.empty()) {
            output += "\n[Full output saved to " + tee_path + "]";
        }
    } else {
        output = filter_tool_output(output);
    }
    return output;
}

} // namespace ptrclaw
