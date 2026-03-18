#include "../tool.hpp"
#include "../event.hpp"
#include "../event_bus.hpp"
#include "../plugin.hpp"
#include "../util.hpp"
#include <nlohmann/json.hpp>
#include <mutex>
#include <condition_variable>

namespace ptrclaw {

class SkillActivateTool : public EventBusAwareTool {
public:
    ToolResult execute(const std::string& args_json) override {
        if (!event_bus_) return {false, "EventBus not available"};

        nlohmann::json args;
        try {
            args = nlohmann::json::parse(args_json);
        } catch (...) {
            return {false, "Invalid JSON arguments"};
        }

        std::string name = args.value("name", "");
        if (name.empty()) {
            return {false, "Missing required parameter: name"};
        }

        // Determine action
        std::string action = "activate";
        if (name == "off" || name == "none") {
            action = "deactivate";
        }

        // Publish request and wait for response
        std::string request_id = generate_id();

        std::mutex mtx;
        std::condition_variable cv;
        bool received = false;
        SkillResponseEvent response;

        uint64_t sub_id = subscribe<SkillResponseEvent>(*event_bus_,
            std::function<void(const SkillResponseEvent&)>(
                [&](const SkillResponseEvent& ev) {
                    if (ev.request_id != request_id) return;
                    std::lock_guard<std::mutex> lock(mtx);
                    response = ev;
                    received = true;
                    cv.notify_one();
                }));

        SkillRequestEvent req;
        req.session_id = session_id_;
        req.request_id = request_id;
        req.action = action;
        req.name = name;
        event_bus_->publish(req);

        {
            std::unique_lock<std::mutex> lock(mtx);
            cv.wait_for(lock, std::chrono::seconds(5),
                [&] { return received; });
        }

        event_bus_->unsubscribe(sub_id);

        if (!received) {
            return {false, "Skill request timed out"};
        }

        return {response.success, response.message};
    }

    std::string tool_name() const override { return "skill_activate"; }

    std::string description() const override {
        return "Activate a skill to change the assistant's behavior mode. "
               "Use name=\"off\" to deactivate.";
    }

    std::string parameters_json() const override {
        return R"({
  "type": "object",
  "properties": {
    "name": {
      "type": "string",
      "description": "Name of the skill to activate, or 'off' to deactivate"
    }
  },
  "required": ["name"]
})";
    }
};

static ToolRegistrar reg_skill_activate("skill_activate",
    []() { return std::make_unique<SkillActivateTool>(); });

} // namespace ptrclaw
