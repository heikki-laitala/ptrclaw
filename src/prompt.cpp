#include "prompt.hpp"
#include "util.hpp"
#include <filesystem>
#include <sstream>

namespace ptrclaw {

std::string build_system_prompt(const std::vector<std::unique_ptr<Tool>>& tools,
                                bool include_tool_descriptions,
                                bool has_memory) {
    std::ostringstream ss;

    ss << "You are PtrClaw, an autonomous AI coding assistant.\n\n";
    ss << "Current date: " << timestamp_now() << "\n";
    ss << "Working directory: " << std::filesystem::current_path().string() << "\n\n";
    ss << "You can use tools to interact with the system. "
       << "When you need to use a tool, respond with the appropriate tool call.\n";

    if (include_tool_descriptions) {
        ss << "\nAvailable tools:\n";
        for (const auto& tool : tools) {
            ss << "- " << tool->tool_name() << ": " << tool->description() << "\n";
            ss << "  Parameters: " << tool->parameters_json() << "\n";
        }
        ss << "\nTo use a tool, wrap your call in XML tags:\n";
        ss << "<tool_call>{\"name\": \"tool_name\", \"arguments\": {...}}</tool_call>\n";
    }

    if (has_memory) {
        ss << "\nYou have persistent memory across sessions. "
           << "Use memory_store to save important facts, preferences, and patterns. "
           << "Use memory_recall to search past memories. "
           << "Use memory_forget to remove outdated memories.\n"
           << "User messages may include a [Memory context] block with recalled memories — "
           << "use this context to provide more relevant responses.\n";
    }

    ss << "\nAlways explain what you're doing before using tools.\n";

    return ss.str();
}

} // namespace ptrclaw
