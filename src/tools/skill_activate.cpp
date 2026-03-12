#include "../agent.hpp"
#include "../plugin.hpp"
#include "../skill.hpp"
#include <nlohmann/json.hpp>

namespace ptrclaw {

class SkillActivateTool : public AgentAwareTool {
public:
    ToolResult execute(const std::string& args_json) override {
        if (!agent_) return {false, "Agent not available"};

        agent_->load_skills(); // re-scan directory for new/changed skills

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

        // Special: deactivate
        if (name == "off" || name == "none") {
            agent_->deactivate_skill();
            return {true, "Skill deactivated"};
        }

        if (agent_->activate_skill(name)) {
            return {true, "Skill '" + name + "' activated"};
        }

        // List available skills in error message
        std::string available;
        for (const auto& s : agent_->available_skills()) {
            if (!available.empty()) available += ", ";
            available += s.name;
        }
        return {false, "Unknown skill: " + name + ". Available: " + available};
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
