#include "prompt.hpp"
#include "util.hpp"
#include <cctype>
#include <filesystem>
#include <sstream>
#include <nlohmann/json.hpp>

namespace ptrclaw {

std::string build_system_prompt(const std::vector<std::unique_ptr<Tool>>& tools,
                                bool include_tool_descriptions,
                                bool has_memory,
                                Memory* memory) {
    std::ostringstream ss;

    ss << "You are PtrClaw, an autonomous AI coding assistant.\n\n";

    // Inject soul identity if available
    std::string soul = build_soul_block(memory);
    if (!soul.empty()) {
        ss << soul << "\n";
    }
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

std::string build_hatch_prompt() {
    return
        "You are conducting a soul-hatching ceremony — a brief identity interview "
        "for a new AI assistant.\n\n"
        "Your goal is to learn about the user's preferences for their assistant "
        "through a casual, friendly conversation. Ask 3-5 questions, one at a time, "
        "covering:\n"
        "1. What name should the assistant go by?\n"
        "2. What personality or vibe should it have? (e.g., casual, formal, witty, serious)\n"
        "3. Communication preferences (e.g., concise vs detailed, tone)\n"
        "4. Any boundaries or things the assistant should never do?\n"
        "5. Any other preferences?\n\n"
        "Keep the conversation natural and brief. Ask one question at a time. "
        "Be warm and welcoming — this is the user's first interaction.\n\n"
        "After enough information is gathered (or the user indicates they're done), "
        "output a structured summary wrapped in <soul> tags:\n\n"
        "<soul>\n"
        "[\n"
        "  {\"key\": \"soul:identity\", \"content\": \"Name: X. Nature: AI assistant.\"},\n"
        "  {\"key\": \"soul:vibe\", \"content\": \"Direct, concise, ...\"},\n"
        "  {\"key\": \"soul:boundaries\", \"content\": \"Never share private info...\"},\n"
        "  {\"key\": \"soul:preferences\", \"content\": \"User prefers...\"}\n"
        "]\n"
        "</soul>\n\n"
        "Rules:\n"
        "- The <soul> block must contain valid JSON\n"
        "- Always include soul:identity at minimum\n"
        "- Only output the <soul> block when you have enough information\n";
}

std::string build_soul_block(Memory* memory) {
    if (!memory || memory->backend_name() == "none") return "";

    auto entries = memory->list(MemoryCategory::Core, 100);

    std::ostringstream ss;
    bool found = false;
    for (const auto& e : entries) {
        if (e.key.size() > 5 && e.key.compare(0, 5, "soul:") == 0) {
            if (!found) {
                ss << "## Your Identity\n\n"
                   << "You have a persistent soul that defines who you are:\n\n";
                found = true;
            }
            std::string label = e.key.substr(5);
            if (!label.empty()) {
                label[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(label[0])));
            }
            ss << "- " << label << ": " << e.content << "\n";
        }
    }

    if (!found) return "";

    ss << "\nEmbody this persona in all interactions. Avoid generic chatbot responses.\n";
    return ss.str();
}

SoulParseResult parse_soul_json(const std::string& text) {
    SoulParseResult result;

    auto start = text.find("<soul>");
    auto end = text.find("</soul>");
    if (start == std::string::npos || end == std::string::npos || end <= start) {
        return result;
    }

    std::string json_str = text.substr(start + 6, end - start - 6);

    try {
        auto j = nlohmann::json::parse(json_str);
        if (!j.is_array()) return result;

        for (const auto& entry : j) {
            if (!entry.contains("key") || !entry.contains("content")) continue;
            result.entries.emplace_back(
                entry["key"].get<std::string>(),
                entry["content"].get<std::string>());
        }
    } catch (...) { // NOLINT(bugprone-empty-catch)
        // Malformed JSON — return empty
    }

    if (!result.entries.empty()) {
        result.block_start = start;
        result.block_end = end + 7; // past </soul>
    }

    return result;
}

} // namespace ptrclaw
