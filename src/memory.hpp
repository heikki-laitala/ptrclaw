#pragma once
#include "tool.hpp"
#include <string>
#include <vector>
#include <optional>
#include <cstdint>
#include <memory>

namespace ptrclaw {

class Embedder; // forward declaration
struct MemoryConfig; // forward declaration

enum class MemoryCategory { Core, Knowledge, Conversation };

struct MemoryEntry {
    std::string id;
    std::string key;
    std::string content;
    MemoryCategory category = MemoryCategory::Knowledge;
    uint64_t timestamp = 0;
    uint64_t last_accessed = 0;
    std::string session_id;
    double score = 0.0;
    std::vector<std::string> links;  // keys of bidirectionally linked entries
};

// Abstract memory backend interface
class Memory {
public:
    virtual ~Memory() = default;

    virtual std::string backend_name() const = 0;

    // Store or upsert a memory entry by key. Returns the entry ID.
    virtual std::string store(const std::string& key,
                              const std::string& content,
                              MemoryCategory category,
                              const std::string& session_id) = 0;

    // Search memories by query string. Returns up to `limit` entries, scored.
    virtual std::vector<MemoryEntry> recall(const std::string& query,
                                            uint32_t limit,
                                            std::optional<MemoryCategory> category_filter) = 0;

    // Get a single entry by exact key match.
    virtual std::optional<MemoryEntry> get(const std::string& key) = 0;

    // List entries, optionally filtered by category.
    virtual std::vector<MemoryEntry> list(std::optional<MemoryCategory> category_filter,
                                          uint32_t limit) = 0;

    // Delete a memory entry by key. Returns true if found and deleted.
    virtual bool forget(const std::string& key) = 0;

    // Count entries, optionally filtered by category.
    virtual uint32_t count(std::optional<MemoryCategory> category_filter) = 0;

    // Export all entries as a JSON string.
    virtual std::string snapshot_export() = 0;

    // Import entries from a JSON string. Returns number imported.
    virtual uint32_t snapshot_import(const std::string& json_str) = 0;

    // Purge conversation entries older than max_age_seconds. Returns count purged.
    virtual uint32_t hygiene_purge(uint32_t max_age_seconds) = 0;

    // Create bidirectional link between two entries. Returns false if either doesn't exist.
    virtual bool link(const std::string& from_key, const std::string& to_key) = 0;

    // Remove bidirectional link. Returns false if link doesn't exist.
    virtual bool unlink(const std::string& from_key, const std::string& to_key) = 0;

    // Get entries linked to the given key, up to limit.
    virtual std::vector<MemoryEntry> neighbors(const std::string& key, uint32_t limit) = 0;

    // Set embedder for vector search (default no-op, backends override if supported).
    // The embedder pointer must outlive this Memory instance.
    // text_weight + vector_weight control hybrid scoring blend.
    virtual void set_embedder(Embedder* /*embedder*/,
                              double /*text_weight*/ = 0.4,
                              double /*vector_weight*/ = 0.6) {}

    // Set recency decay half-life in seconds (0 = disabled).
    // Scores are multiplied by exp(-ln(2) * age / half_life).
    virtual void set_recency_decay(uint32_t /*half_life_seconds*/) {}

    // Set knowledge decay parameters (0 max_idle_days = disabled).
    virtual void set_knowledge_decay(uint32_t /*max_idle_days*/,
                                     double /*survival_chance*/) {}

    // Apply all config-driven settings at once (recency decay, knowledge decay, etc.).
    // Backends override to extract the fields they care about.
    virtual void apply_config(const MemoryConfig& /*cfg*/) {}
};

// Base class for tools that need a Memory* pointer.
// Agent wires this up after construction.
class MemoryAwareTool : public Tool {
public:
    void set_memory(Memory* mem) { memory_ = mem; }

protected:
    Memory* memory_ = nullptr;
};

// Category string conversions
std::string category_to_string(MemoryCategory cat);
MemoryCategory category_from_string(const std::string& s);

// Enrich a user message with recalled memory context.
// Returns the enriched message (original message with prepended context),
// or the original message unchanged if memory is null or recall returns nothing.
// Follow 1-hop links from the given entries, deduplicating by key.
// Returns only the neighbor entries not already present in `entries`.
std::vector<MemoryEntry> collect_neighbors(Memory* memory,
                                            const std::vector<MemoryEntry>& entries,
                                            uint32_t limit);

std::string memory_enrich(Memory* memory, const std::string& user_message,
                          uint32_t recall_limit, uint32_t enrich_depth = 0);

// Create a memory backend from config.
// Uses the plugin registry to instantiate the configured backend.
struct Config;
std::unique_ptr<Memory> create_memory(const Config& config);

} // namespace ptrclaw
