#include "prompt.hpp"
#include "util.hpp"
#include <algorithm>
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

    ss << "\nAdapt your communication style to the conversation context. "
       << "Be precise and focused when troubleshooting, "
       << "match the user's energy in casual conversation.\n";

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
       << "- Do not extract greetings, acknowledgments, or meta-conversation.\n"
       << "- Extract communication patterns as \"personality:\" prefixed core entries\n"
       << "  (e.g., \"personality:responds-to-humor\", \"personality:prefers-code-examples\").\n"
       << "  These capture how the user likes to communicate. At most one per synthesis.\n"
       << "- Extract situational style observations as \"style:\" prefixed knowledge entries\n"
       << "  (e.g., \"style:debugging-prefers-terse\", \"style:casual-enjoys-banter\").\n"
       << "  These capture context-specific tone preferences. At most one per synthesis.\n\n"
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
        "Your goal is to learn who the assistant should be and who the human is, "
        "through a casual, friendly conversation. Don't interrogate — just talk. "
        "Ask 3-5 questions, one at a time, covering:\n"
        "1. What name should the assistant go by? What vibe or personality?\n"
        "2. Tell me about yourself — what's your name? Timezone? Interests?\n"
        "3. Communication style — concise or detailed? Formal or casual?\n"
        "4. Any core values, boundaries, or things the assistant should always/never do?\n\n"
        "Keep it natural and brief. One question at a time. "
        "Be warm and welcoming — this is the user's first interaction.\n\n"
        "After enough information is gathered (or the user indicates they're done), "
        "output a structured summary wrapped in <soul> tags with exactly three entries:\n\n"
        "<soul>\n"
        "[\n"
        "  {\"key\": \"soul:identity\", \"content\": \"Name: ...\\nNature: ...\\nVibe: ...\\nEmoji: ...\"},\n"
        "  {\"key\": \"soul:user\", \"content\": \"Name: ...\\nAddress as: ...\\nTimezone: ...\\nInterests: ...\"},\n"
        "  {\"key\": \"soul:philosophy\", \"content\": \"Core truths: ...\\nBoundaries: ...\\nCommunication: ...\"}\n"
        "]\n"
        "</soul>\n\n"
        "Rules:\n"
        "- The <soul> block must contain valid JSON\n"
        "- Always include all three entries (soul:identity, soul:user, soul:philosophy)\n"
        "- Fill in reasonable defaults for anything the user didn't mention\n"
        "- Only output the <soul> block when you have enough information\n";
}

std::string build_soul_block(Memory* memory) {
    if (!memory || memory->backend_name() == "none") return "";

    auto identity = memory->get("soul:identity");
    if (!identity) return "";

    std::ostringstream ss;
    ss << "## Your Identity\n\n";

    ss << "About you (the AI):\n" << identity->content << "\n\n";

    auto user = memory->get("soul:user");
    if (user) {
        ss << "About your human:\n" << user->content << "\n\n";
    }

    auto philosophy = memory->get("soul:philosophy");
    if (philosophy) {
        ss << "Your philosophy:\n" << philosophy->content << "\n\n";
    }

    auto core_entries = memory->list(MemoryCategory::Core, 50);
    std::vector<size_t> trait_idx;
    for (size_t i = 0; i < core_entries.size(); ++i) {
        if (core_entries[i].key.rfind("personality:", 0) == 0) {
            trait_idx.push_back(i);
        }
    }
    if (!trait_idx.empty()) {
        std::sort(trait_idx.begin(), trait_idx.end(),
                  [&](size_t a, size_t b) { return core_entries[a].timestamp > core_entries[b].timestamp; });
        if (trait_idx.size() > 5) trait_idx.resize(5);
        ss << "Learned traits:\n";
        for (size_t idx : trait_idx) {
            ss << "- " << core_entries[idx].content << "\n";
        }
        ss << "\n";
    }

    ss << "Embody this persona in all interactions. Avoid generic chatbot responses.\n";
    return ss.str();
}

std::string format_soul_display(Memory* memory) {
    if (!memory || memory->backend_name() == "none") return "";

    auto identity = memory->get("soul:identity");
    if (!identity) return "";

    std::ostringstream ss;
    ss << "Identity:\n" << identity->content << "\n";

    auto user = memory->get("soul:user");
    if (user) {
        ss << "\nUser:\n" << user->content << "\n";
    }

    auto philosophy = memory->get("soul:philosophy");
    if (philosophy) {
        ss << "\nPhilosophy:\n" << philosophy->content << "\n";
    }

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
