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

        // The lambda is passed straight in: the explicit
        // std::function<void(const SkillResponseEvent&)> wrapper this used to build
        // was redundant, since naming the event type in subscribe<> already fixes
        // the parameter type. Dropping it removes a heap-allocating temporary that
        // was constructed only to be moved into the parameter — and which the
        // static analyser reported as a leak because it could not follow the
        // ownership handoff into the bus. Matches how session.cpp subscribes.
        uint64_t sub_id = subscribe<SkillResponseEvent>(*event_bus_,
            [&](const SkillResponseEvent& ev) {
                if (ev.request_id != request_id) return;
                std::lock_guard<std::mutex> lock(mtx);
                response = ev;
                received = true;
                cv.notify_one();
            });

        // Unsubscribe on every exit path, not only the happy one. The handler
        // captures the locals above by reference, so if publish() or wait_for()
        // threw, the bus would keep a callback pointing at destroyed stack objects
        // — and leak the std::function's storage along with it.
        struct ScopedUnsubscribe {
            EventBus* bus;
            uint64_t id;
            ~ScopedUnsubscribe() { bus->unsubscribe(id); }
        } scoped_unsubscribe{event_bus_, sub_id};

        SkillRequestEvent req;
        req.session_id = session_id_;
        req.request_id = request_id;
        req.action = action;
        req.name = name;
        event_bus_->publish(req);

        // Copy the result out while holding the lock. Previously the unsubscribe
        // call sat here and acted as the barrier that made an unsynchronized read
        // of `response` safe; now that unsubscribing happens at scope exit, a late
        // event could otherwise be writing `response` while we read it.
        bool got_response = false;
        SkillResponseEvent result;
        {
            std::unique_lock<std::mutex> lock(mtx);
            cv.wait_for(lock, std::chrono::seconds(5),
                [&] { return received; });
            got_response = received;
            result = response;
        }

        if (!got_response) {
            return {false, "Skill request timed out"};
        }

        return {result.success, result.message};
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
