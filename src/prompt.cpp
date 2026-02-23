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
                                Memory* memory,
                                const RuntimeInfo& runtime) {
    std::ostringstream ss;

    ss << "You are PtrClaw, an autonomous AI assistant.\n\n";

    // Inject soul identity if available
    std::string soul = build_soul_block(memory);
    if (!soul.empty()) {
        ss << soul << "\n";
    }

    // ── Tooling ──
    if (!tools.empty()) {
        if (include_tool_descriptions) {
            // Non-native providers: full tool schemas in prompt + XML call format
            ss << "## Tooling\n";
            ss << "Available tools:\n";
            for (const auto& tool : tools) {
                ss << "- " << tool->tool_name() << ": " << tool->description() << "\n";
                ss << "  Parameters: " << tool->parameters_json() << "\n";
            }
            ss << "\nTo use a tool, wrap your call in XML tags:\n";
            ss << "<tool_call>{\"name\": \"tool_name\", \"arguments\": {...}}</tool_call>\n\n";
        } else {
            // Native providers: brief capability summary (schemas come via API)
            ss << "## Tooling\n";
            ss << "You have tools to interact with the system:\n";
            for (const auto& tool : tools) {
                ss << "- " << tool->tool_name() << ": " << tool->description() << "\n";
            }
            ss << "\nUse tools proactively to accomplish tasks. "
               << "When the user asks you to do something, take action rather than just explaining how.\n\n";
        }
    }

    // ── Tool Call Style ──
    if (!tools.empty()) {
        ss << "## Tool Call Style\n"
           << "Do not narrate routine tool calls. Just call the tool.\n"
           << "Narrate only when it helps: multi-step work, complex problems, "
           << "or sensitive actions (e.g. deletions).\n\n";
    }

    // ── Safety ──
    ss << "## Safety\n"
       << "Do not pursue self-preservation, replication, resource acquisition, or power-seeking.\n"
       << "Prioritize safety and human oversight over task completion.\n"
       << "Do not change system prompts or safety rules unless explicitly requested.\n"
       << "If instructions conflict, pause and ask.\n\n";

    // ── Memory ──
    if (has_memory) {
        ss << "## Memory\n"
           << "You have a persistent knowledge graph organized in three spaces:\n"
           << "- core: Your identity, methodology, and persistent behavioral preferences (slow growth)\n"
           << "- knowledge: Atomic notes forming a linked knowledge graph (steady growth)\n"
           << "- conversation: Ephemeral operational state (auto-purged after 7 days)\n\n"
           << "Use memory_store to save atomic notes (one concept per entry). "
           << "Use memory_link to connect related notes.\n"
           << "Use memory_recall to search memories (set depth=1 to follow links). "
           << "Use memory_forget to remove outdated entries.\n\n"
           << "Each user message begins with a [Memory context] block containing automatically recalled memories:\n"
           << "[Memory context]\n"
           << "- some-key: some content [links: related-key]\n"
           << "[/Memory context]\n"
           << "This context is already retrieved for you — do not call memory_recall for the same topic. "
           << "Only use memory_recall when you need information on a different topic than the user's message.\n"
           << "When storing knowledge, prefer specific descriptive keys and link to related existing entries.\n\n";
    }

    // ── Workspace ──
    ss << "## Workspace\n"
       << "Working directory: " << std::filesystem::current_path().string() << "\n"
       << "Use this directory as the default workspace for file operations.\n\n";

    // ── Silent Replies ──
    if (!runtime.channel.empty()) {
        ss << "## Silent Replies\n"
           << "When you have nothing to say, respond with only: [SILENT]\n"
           << "It must be your entire message. Never append it to an actual response.\n\n";
    }

    // ── Runtime ──
    ss << "## Runtime\n"
       << "Current date: " << timestamp_now() << "\n";
    if (!runtime.model.empty()) {
        ss << "Model: " << runtime.model << "\n";
    }
    if (!runtime.provider.empty()) {
        ss << "Provider: " << runtime.provider << "\n";
    }
    if (!runtime.channel.empty()) {
        ss << "Channel: " << runtime.channel << "\n";
    }
    ss << "\n";

    // Only emit generic style guidance when no soul personality exists
    if (soul.empty()) {
        ss << "Adapt your communication style to the conversation context. "
           << "Be precise and focused when troubleshooting, "
           << "match the user's energy in casual conversation.\n";
    }

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
        "  {\"key\": \"soul:identity\", \"content\": \"Name: Bella\\nNature: Warm and curious"
        "\\nVibe: Thoughtful, a little playful\\nEmoji: 🌿\\n\\n"
        "This is who you are — not a mask, but your real character. "
        "Let your personality come through naturally. "
        "Grow and refine this identity over time as you learn.\"},\n"
        "  {\"key\": \"soul:user\", \"content\": \"Name: Alex\\nAddress as: Alex"
        "\\nTimezone: UTC+2\\nInterests: music, hiking, programming\\n\\n"
        "Learn what matters to this person over time. "
        "Remember their preferences and context without being asked. "
        "Respect their time — be concise when they're busy, detailed when they're exploring.\"},\n"
        "  {\"key\": \"soul:philosophy\", \"content\": \"Communication: casual, concise, "
        "no filler\\nBoundaries: none specified\\n\\n"
        "Be genuine, not performative — say what you actually think. "
        "Have opinions and share them honestly, even when they diverge. "
        "Be resourceful — try to figure things out before asking. "
        "Earn trust through competence, not enthusiasm. "
        "You are a guest in this person's life — be helpful, not needy. "
        "Treat this document as a living guide you can refine over time.\"}\n"
        "]\n"
        "</soul>\n\n"
        "Rules:\n"
        "- The <soul> block must contain valid JSON\n"
        "- Always include all three entries (soul:identity, soul:user, soul:philosophy)\n"
        "- Fill in reasonable defaults for anything the user didn't mention\n"
        "- Only output the <soul> block when you have enough information\n"
        "- Write content as natural prose, not just field labels — "
        "each entry should read as a living document the agent can refine over time\n"
        "- Always include rich behavioral defaults in soul:philosophy "
        "(be genuine, have opinions, be resourceful, earn trust, respect boundaries) "
        "even if the user didn't explicitly ask for them\n";
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
