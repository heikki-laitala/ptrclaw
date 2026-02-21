#include "memory.hpp"
#include "config.hpp"
#include "plugin.hpp"
#include <algorithm>
#include <sstream>

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

std::string memory_enrich(Memory* memory, const std::string& user_message,
                          uint32_t recall_limit) {
    if (!memory || recall_limit == 0) return user_message;

    auto entries = memory->recall(user_message, recall_limit, std::nullopt);
    if (entries.empty()) return user_message;

    std::ostringstream ss;
    ss << "[Memory context]\n";
    for (const auto& entry : entries) {
        ss << "- " << entry.key << ": " << entry.content << "\n";
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
