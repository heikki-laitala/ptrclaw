#include "memory.hpp"
#include "config.hpp"
#include "plugin.hpp"
#include <algorithm>
#include <cctype>
#include <sstream>
#include <unordered_set>

namespace ptrclaw {

std::string category_to_string(MemoryCategory cat) {
    switch (cat) {
        case MemoryCategory::Core:         return "core";
        case MemoryCategory::Knowledge:    return "knowledge";
        case MemoryCategory::Conversation: return "conversation";
    }
    return "knowledge";
}

MemoryCategory category_from_string(const std::string& s) {
    if (s == "core")         return MemoryCategory::Core;
    if (s == "conversation") return MemoryCategory::Conversation;
    return MemoryCategory::Knowledge;
}

QueryIntent classify_query_intent(const std::string& query) {
    std::string lower;
    lower.reserve(query.size());
    for (unsigned char c : query) {
        lower.push_back(static_cast<char>(std::tolower(c)));
    }

    // Chronological signals: query about recent events or temporal history
    static const char* const chron_kws[] = {
        "recently", "earlier", "before", "last time", "yesterday",
        "today", "what happened", "when did", " ago", "history", "previously"
    };
    for (const auto* kw : chron_kws) {
        if (lower.find(kw) != std::string::npos) return QueryIntent::Chronological;
    }

    // Stable-fact signals: query about persistent preferences or general knowledge
    static const char* const stable_kws[] = {
        "prefer", "favorite", "favourite", "always", "usually",
        "typically", "generally", "i like", "i use"
    };
    for (const auto* kw : stable_kws) {
        if (lower.find(kw) != std::string::npos) return QueryIntent::Stable;
    }

    return QueryIntent::Unknown;
}

std::vector<MemoryEntry> collect_neighbors(Memory* memory,
                                            const std::vector<MemoryEntry>& entries,
                                            uint32_t limit) {
    if (!memory) return {};

    // Track visited keys to prevent cycles and dedup
    std::unordered_set<std::string> seen_keys;
    seen_keys.reserve(entries.size());
    for (const auto& e : entries) {
        seen_keys.insert(e.key);
    }

    std::vector<MemoryEntry> result;
    for (const auto& entry : entries) {
        if (entry.links.empty()) continue;
        auto neighbors = memory->neighbors(entry.key, limit);
        for (auto& n : neighbors) {
            if (seen_keys.insert(n.key).second) {
                result.push_back(std::move(n));
            }
        }
    }
    return result;
}

std::string memory_enrich(Memory* memory, const std::string& user_message,
                          uint32_t recall_limit, uint32_t enrich_depth,
                          const std::string& episode_context) {
    // Compute concept budget hint based on query intent (PER-395).
    // Chronological queries favor observations; stable-fact queries favor concepts;
    // unknown queries split evenly. Dynamic reallocation below ensures no slots
    // are wasted when one tier is underpopulated.
    uint32_t concept_budget = 0;
    if (recall_limit > 0) {
        QueryIntent intent = classify_query_intent(user_message);
        switch (intent) {
            case QueryIntent::Chronological:
                concept_budget = recall_limit / 3;
                break;
            case QueryIntent::Stable:
                concept_budget = recall_limit - recall_limit / 3;
                break;
            default:
                concept_budget = recall_limit / 2;
                break;
        }
    }

    // Collect recalled entries when memory is available
    std::vector<MemoryEntry> entries;
    if (memory && recall_limit > 0) {
        // Over-fetch to compensate for Core entries we'll filter out (they're in the system prompt)
        entries = memory->recall(user_message, recall_limit * 2, std::nullopt);
        entries.erase(std::remove_if(entries.begin(), entries.end(),
            [](const MemoryEntry& e) { return e.category == MemoryCategory::Core; }),
            entries.end());
        // Do not truncate here — tier budgets below handle the limits independently.
    }

    std::vector<MemoryEntry> neighbor_entries;
    if (enrich_depth > 0 && memory && !entries.empty()) {
        neighbor_entries = collect_neighbors(memory, entries, recall_limit);
    }

    // Split into concepts (cross-session, session_id empty) and
    // observations (session-specific, session_id set). Neighbors follow the same rule.
    std::vector<const MemoryEntry*> concepts, observations;
    for (const auto& e : entries) {
        (e.session_id.empty() ? concepts : observations).push_back(&e);
    }
    for (const auto& e : neighbor_entries) {
        (e.session_id.empty() ? concepts : observations).push_back(&e);
    }

    // Sort each tier by relevance score descending so the highest-value entries
    // appear first and low-relevance items are dropped when the budget is tight.
    auto by_score_desc = [](const MemoryEntry* a, const MemoryEntry* b) {
        return a->score > b->score;
    };
    // NOLINTBEGIN(bugprone-nondeterministic-pointer-iteration-order)
    // Comparator uses score, not pointer values — order is deterministic.
    std::sort(concepts.begin(), concepts.end(), by_score_desc);
    std::sort(observations.begin(), observations.end(), by_score_desc);
    // NOLINTEND(bugprone-nondeterministic-pointer-iteration-order)

    // Apply tier budgets with dynamic reallocation: if one tier is
    // underpopulated, give its unused slots to the other tier so we
    // always use up to recall_limit entries when available.
    auto concept_count = static_cast<uint32_t>(concepts.size());
    auto obs_count     = static_cast<uint32_t>(observations.size());
    uint32_t concepts_shown = std::min(concept_count, concept_budget);
    uint32_t remaining      = recall_limit - concepts_shown;
    uint32_t obs_shown      = std::min(obs_count, remaining);
    // Give back unused observation slots to concepts
    uint32_t final_concept  = std::min(concept_count, recall_limit - obs_shown);
    concepts.resize(final_concept);
    observations.resize(obs_shown);

    bool has_content = !concepts.empty() || !observations.empty() || !episode_context.empty();
    if (!has_content) return user_message;

    auto format_links = [](std::ostringstream& out, const MemoryEntry* e) {
        if (!e->links.empty()) {
            out << " [links: ";
            for (size_t i = 0; i < e->links.size(); i++) {
                if (i > 0) out << ", ";
                out << e->links[i];
            }
            out << "]";
        }
    };

    std::ostringstream ss;
    ss << "[Memory context]\n";

    if (!concepts.empty()) {
        ss << "Concepts:\n";
        for (const auto* e : concepts) {
            ss << "- " << e->key << ": " << e->content;
            format_links(ss, e);
            ss << "\n";
        }
    }

    if (!observations.empty()) {
        ss << "Observations:\n";
        for (const auto* e : observations) {
            ss << "- " << e->key << ": " << e->content;
            format_links(ss, e);
            ss << "\n";
        }
    }

    if (!episode_context.empty()) {
        ss << episode_context << "\n";
    }

    ss << "[/Memory context]\n\n" << user_message;
    return ss.str();
}

std::unique_ptr<Memory> create_memory(const Config& config) {
    const auto& backend = config.memory.backend;
    auto& registry = PluginRegistry::instance();

    if (!registry.has_memory(backend)) {
        // Fall back to "none" if configured backend is missing
        if (registry.has_memory("none")) {
            return registry.create_memory("none", config);
        }
        return nullptr;
    }

    return registry.create_memory(backend, config);
}

} // namespace ptrclaw
