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
        ss << "\nYou have a persistent knowledge graph organized in three spaces:\n"
           << "- core: Your identity, methodology, and persistent behavioral preferences (slow growth)\n"
           << "- knowledge: Atomic notes forming a linked knowledge graph (steady growth)\n"
           << "- conversation: Ephemeral operational state (auto-purged after 7 days)\n\n"
           << "Use memory_store to save atomic notes (one concept per entry). "
           << "Use memory_link to connect related notes.\n"
           << "Use memory_recall to search memories (set depth=1 to follow links). "
           << "Use memory_forget to remove outdated entries.\n\n"
           << "User messages may include a [Memory context] block with recalled memories and their links.\n"
           << "When storing knowledge, prefer specific descriptive keys and link to related existing entries.\n";
    }

    ss << "\nAlways explain what you're doing before using tools.\n";

    return ss.str();
}

std::string build_synthesis_prompt(const std::vector<ChatMessage>& history,
                                    const std::vector<MemoryEntry>& existing_entries) {
    std::ostringstream ss;

    ss << "Extract atomic knowledge notes from the following conversation.\n\n"
       << "Rules:\n"
       << "- Each note should capture exactly one fact, claim, or preference.\n"
       << "- Use concise, descriptive keys (e.g., \"user-prefers-python\", \"project-uses-cmake\").\n"
       << "- Category must be \"core\" (identity/behavior) or \"knowledge\" (factual).\n"
       << "- Suggest links to existing entries when related.\n"
       << "- Prefer fewer high-quality notes over many trivial ones.\n"
       << "- Do not extract greetings, acknowledgments, or meta-conversation.\n\n"
       << "Output a JSON array: [{\"key\":\"...\",\"content\":\"...\",\"category\":\"...\",\"links\":[\"...\"]}]\n"
       << "Output ONLY the JSON array, no other text.\n\n";

    if (!existing_entries.empty()) {
        ss << "Existing memory entries (for linking):\n";
        for (const auto& e : existing_entries) {
            ss << "- " << e.key << ": " << e.content << "\n";
        }
        ss << "\n";
    }

    ss << "Conversation:\n";
    for (const auto& msg : history) {
        if (msg.role == Role::User) {
            ss << "User: " << msg.content << "\n";
        } else if (msg.role == Role::Assistant) {
            ss << "Assistant: " << msg.content << "\n";
        }
    }

    return ss.str();
}

} // namespace ptrclaw
