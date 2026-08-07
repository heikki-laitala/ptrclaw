#pragma once
#include "config.hpp"
#include "memory.hpp"
#include "provider.hpp"
#include "tool.hpp"
#include <string>
#include <vector>
#include <utility>

namespace ptrclaw {

struct RuntimeInfo {
    std::string model;
    std::string provider;
    std::string channel;      // empty if CLI
    std::string binary_path;  // resolved absolute path to ptrclaw binary
    std::string session_id;   // current session ID (e.g. telegram chat ID)
    // Filesystem scope, when the build and config give the session one. Empty on the
    // personal agent, where tools reach whatever the process can and the prompt says so.
    std::string workspace;    // this session's own directory: read and write
    std::string context_dir;  // shared across sessions: read-only
};

// Build the system prompt, including tool descriptions for XML-based providers.
// Tool specs are pre-filtered (e.g. memory tools omitted when inactive).
// When has_memory is true, includes instructions about memory tools and context format.
// When memory is non-null, injects soul identity block if soul entries exist.
// `persona`, when set, supplies the identity block instead of memory. One source of truth:
// a deployment that states who the agent is should not also depend on what a per-session
// store happens to contain.
std::string build_system_prompt(const std::vector<ToolSpec>& tool_specs,
                                bool include_tool_descriptions,
                                bool has_memory = false,
                                Memory* memory = nullptr,
                                const RuntimeInfo& runtime = {},
                                const PersonaConfig* persona = nullptr);

// Build the hatching bootstrap system prompt for soul creation.
std::string build_hatch_prompt();

// Build a soul injection block from core memory entries for the system prompt.
// Returns empty string if no soul entries exist.
std::string build_soul_block(Memory* memory);

// The same block from a configured persona. Kept byte-identical in shape to the memory
// version so the two are indistinguishable downstream.
std::string build_soul_block(const PersonaConfig& persona);

// Format soul data for user-facing display (e.g. /soul command).
// Returns empty string if no soul entries exist.
std::string format_soul_display(Memory* memory);

// Result of parsing <soul>...</soul> tags from text.
struct SoulParseResult {
    std::vector<std::pair<std::string, std::string>> entries; // key/content pairs
    size_t block_start = std::string::npos; // position of '<' in <soul>
    size_t block_end = std::string::npos;   // position past '>' in </soul>
    bool found() const { return !entries.empty(); }
};

// Extract and parse soul entries from a response containing <soul>...</soul> tags.
SoulParseResult parse_soul_json(const std::string& text);

// Build synthesis prompt to extract atomic notes from conversation history.
std::string build_synthesis_prompt(const std::vector<ChatMessage>& history,
                                    const std::vector<MemoryEntry>& existing_entries);

} // namespace ptrclaw
