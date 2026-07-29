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

        // The handler's state lives in one object so the lambda captures a single
        // reference. That is not cosmetic: libc++'s std::function only stores a
        // callable inline when it fits in __buf_ (three pointers), and otherwise
        // heap-allocates. A `[&]` capture of four separate locals exceeded that and
        // allocated on every skill activation — which is also what the static
        // analyser reported as a leak, since it cannot follow the allocation's
        // ownership through subscribe<>'s re-wrap into the bus. One reference fits
        // inline, so no allocation happens at all. session.cpp captures `[this]`
        // for the same reason and has always been clean.
        struct ResponseWaiter {
            std::mutex mtx;
            std::condition_variable cv;
            bool received = false;
            SkillResponseEvent response;
            std::string request_id;
        };
        ResponseWaiter waiter;
        waiter.request_id = request_id;

        uint64_t sub_id = subscribe<SkillResponseEvent>(*event_bus_,
            [&waiter](const SkillResponseEvent& ev) {
                if (ev.request_id != waiter.request_id) return;
                std::lock_guard<std::mutex> lock(waiter.mtx);
                waiter.response = ev;
                waiter.received = true;
                waiter.cv.notify_one();
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
        // of the response safe; now that unsubscribing happens at scope exit, a late
        // event could otherwise be writing it while we read.
        bool got_response = false;
        SkillResponseEvent result;
        {
            std::unique_lock<std::mutex> lock(waiter.mtx);
            waiter.cv.wait_for(lock, std::chrono::seconds(5),
                [&waiter] { return waiter.received; });
            got_response = waiter.received;
            result = waiter.response;
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
