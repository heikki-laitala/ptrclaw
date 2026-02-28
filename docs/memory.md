# Memory & Context Handling

PtrClaw's memory system gives the agent persistent, cross-session knowledge through a pluggable storage backend, a knowledge graph, automatic context enrichment, and periodic synthesis of conversation into atomic notes.

## Architecture overview

```
Agent::process(user_message)
│
├─ memory_enrich()           ← recall + link-follow, prepend [Memory context] block
│
├─ inject_system_prompt()    ← build_soul_block() injects identity from Core entries
│
├─ ResponseCache::get()      ← FNV-1a hash lookup, skip provider on hit
│
├─ Provider::chat()          ← LLM may call memory tools during tool loop
│   ├─ memory_store
│   ├─ memory_recall
│   ├─ memory_forget
│   └─ memory_link
│
├─ maybe_synthesize()        ← every N turns, extract atomic notes from conversation
│
├─ compact_history()         ← trim history when token/message limit approached
│   ├─ run_synthesis()       ← force synthesis before discarding messages
│   └─ hygiene_purge()       ← delete old Conversation entries
│
└─ ResponseCache::put()      ← cache the response for deduplication
```

## Memory interface

Defined in `src/memory.hpp`. All backends implement this abstract class.

### Types

```cpp
enum class MemoryCategory { Core, Knowledge, Conversation };

struct MemoryEntry {
    std::string id;                    // unique identifier (UUID-style)
    std::string key;                   // human-readable, upsertable
    std::string content;               // the actual data
    MemoryCategory category;           // Core | Knowledge | Conversation
    uint64_t timestamp;                // epoch seconds
    uint64_t last_accessed;            // epoch seconds, updated on store/recall
    std::string session_id;            // optional session scoping
    double score;                      // relevance (set during recall)
    std::vector<std::string> links;    // keys of bidirectionally linked entries
};
```

**Categories:**

| Category | Purpose | Lifecycle |
|----------|---------|-----------|
| `Core` | Identity, personality, soul entries | Permanent. Never auto-purged. |
| `Knowledge` | Learned facts, atomic notes | Subject to knowledge decay (see below). |
| `Conversation` | Transient operational state | Auto-purged after `hygiene_max_age` (default 7 days). |

### Interface methods

| Method | Description |
|--------|-------------|
| `store(key, content, category, session_id)` | Upsert by key. Returns entry ID. |
| `recall(query, limit, category_filter?)` | Full-text search. Returns scored entries. |
| `get(key)` | Exact key lookup. |
| `list(category_filter?, limit)` | List entries, optionally filtered. |
| `forget(key)` | Delete by key. Cleans up dangling links. |
| `count(category_filter?)` | Count entries. |
| `snapshot_export()` | Export all entries as JSON string. |
| `snapshot_import(json_str)` | Import entries, skip duplicates by key. |
| `hygiene_purge(max_age_seconds)` | Delete old Conversation entries + idle Knowledge entries (with random survival). |
| `link(from_key, to_key)` | Bidirectional link between two entries. |
| `unlink(from_key, to_key)` | Remove bidirectional link. |
| `neighbors(key, limit)` | Get entries linked to the given key. |

## Storage backends

### JsonMemory (`src/memory/json_memory.hpp`)

Default backend when SQLite is not available. Zero external dependencies.

- **Storage:** In-memory `std::vector<MemoryEntry>` with `std::unordered_map<key, index>` for O(1) lookups, persisted to `~/.ptrclaw/memory.json`.
- **Writes:** Atomic via temp file + rename (`atomic_write_file()`).
- **Search:** Word-boundary tokenization (lowercase, split on non-alphanumeric). Scores by token hits with 2× weight for key matches vs content matches. Uses `partial_sort` for top-N extraction.
- **Thread safety:** `std::mutex` on all public methods.

Suitable for small-to-medium memory sizes. Entire dataset is held in RAM.

### SqliteMemory (`src/memory/sqlite_memory.hpp`)

Optional backend with full-text search. Requires `sqlite3` at compile time.

**Schema:**

```sql
CREATE TABLE memories (
    id TEXT PRIMARY KEY,
    key TEXT UNIQUE NOT NULL,
    content TEXT NOT NULL,
    category TEXT NOT NULL,
    timestamp INTEGER NOT NULL,
    session_id TEXT NOT NULL,
    embedding BLOB,
    last_accessed INTEGER
);

CREATE VIRTUAL TABLE memories_fts USING fts5(key, content,
    content=memories, content_rowid=rowid);

-- Triggers keep FTS index in sync on INSERT/UPDATE/DELETE

CREATE TABLE memory_links (
    from_key TEXT NOT NULL,
    to_key TEXT NOT NULL,
    PRIMARY KEY (from_key, to_key)
);
```

- **Search:** BM25 ranking via FTS5 (tokens ≥ 2 chars, OR-joined). Falls back to `LIKE` when FTS yields no results or all tokens are single-char. Empty queries return immediately.
- **Performance pragmas:** `journal_mode=WAL`, `synchronous=NORMAL`, `temp_store=MEMORY`.
- **Thread safety:** `std::mutex` + RAII `StmtGuard` for prepared statements.

Suitable for large memory sizes. Disk-backed with efficient indexing.

### NoneMemory (`src/memory/none_memory.hpp`)

No-op backend. All operations return empty/false/zero. Used when memory is disabled.

### Backend selection

Backends self-register via `MemoryRegistrar` in the plugin registry (`src/plugin.hpp`). The factory function `create_memory(config)` looks up the configured backend name and falls back to `"none"` if unavailable.

## Knowledge graph

Entries can be linked bidirectionally. Linking `A → B` also creates `B → A`.

```
user-prefers-python ←→ project-uses-python
                    ←→ python-testing-approach
```

**Operations:**

- `link(from_key, to_key)` — creates both directions. Returns `false` if either entry doesn't exist.
- `unlink(from_key, to_key)` — removes both directions.
- `neighbors(key, limit)` — returns entries whose keys appear in the source's links.
- `forget(key)` — automatically cleans up all dangling references in other entries.

The `collect_neighbors()` helper performs 1-hop traversal with O(1) deduplication via `unordered_set`, used during context enrichment.

## Memory tools

Four LLM-accessible tools extend `MemoryAwareTool`. The agent wires a `Memory*` pointer into each tool during initialization via `set_memory()`.

### memory_store

Store or upsert a memory entry.

```json
{
    "key": "python-version",
    "content": "Project uses Python 3.12",
    "category": "knowledge",
    "links": ["project-setup"]
}
```

- `key` (required): Unique identifier, upsertable.
- `content` (required): Entry text.
- `category` (optional): `core`, `knowledge`, or `conversation`. Default: `knowledge`.
- `links` (optional): Array of keys to link to after storing.

### memory_recall

Search memories by query.

```json
{
    "query": "Python version",
    "limit": 5,
    "category": "knowledge",
    "depth": 1
}
```

- `query` (required): Search text.
- `limit` (optional): Max results. Default: 5.
- `category` (optional): Filter by category.
- `depth` (optional): `0` = flat results, `1` = follow links to include neighbors.

### memory_forget

Delete a memory entry.

```json
{
    "key": "python-version"
}
```

### memory_link

Create or remove bidirectional links.

```json
{
    "action": "link",
    "from": "python-version",
    "to": "project-setup"
}
```

- `action` (required): `link` or `unlink`.
- `from`, `to` (required): Entry keys.

## Context enrichment

On every user message, before it's added to history, `memory_enrich()` runs:

1. `recall(user_message, recall_limit * 2)` — over-fetches to compensate for entries filtered in step 2.
2. **Filter out Core entries** — they're already injected into the system prompt via `build_soul_block()`, so including them would be redundant duplication.
3. **Trim to `recall_limit`** — cap the remaining entries to the configured limit.
4. If `enrich_depth > 0`: `collect_neighbors()` — follow links 1 hop, deduplicate.
5. Prepend a `[Memory context]...[/Memory context]` block to the user message.

If no matching Knowledge or Conversation entries are found (or memory is null/disabled), the message is passed through unchanged — **no empty block is added**.

Enrichment is skipped entirely during soul hatching.

**Example enriched message:**

```
[Memory context]
- user-prefers-python: Prefers Python for scripting [links: project-uses-python]
- project-uses-python: Current project uses Python 3.12
[/Memory context]

How do I set up the development environment?
```

The system prompt instructs the LLM not to redundantly recall topics already present in the context block. The `strip_memory_context()` helper removes these blocks from messages before synthesis, so extracted notes don't contain stale context metadata.

## Soul hatching

An interactive personality-creation flow that bootstraps the agent's identity.

### Flow

1. User triggers hatching via `/start` (first interaction) or `/hatch`.
2. Agent enters hatching mode (`hatching_ = true`). No tools are offered.
3. System prompt switches to `build_hatch_prompt()` — guides the LLM to ask the user about identity, preferences, and communication style.
4. After sufficient conversation, the LLM outputs a `<soul>...</soul>` JSON block.
5. `parse_soul_json()` extracts three key/content pairs:
   - `soul:identity` — name and personality
   - `soul:user` — human context and preferences
   - `soul:philosophy` — interaction principles
6. Entries are stored in Core category. Synthesis runs on the hatching conversation.
7. Hatching mode ends. History is cleared. System prompt is re-injected with soul block.

### Soul injection

On every system prompt build, `build_soul_block()` checks for Core entries:

```
## Your Identity
{soul:identity content}

## About your human
{soul:user content}

## Your philosophy
{soul:philosophy content}

## Learned traits
- {personality:humor content}
- {personality:directness content}
```

Up to 5 `personality:*` entries are included, sorted by timestamp (newest first).

## Synthesis

Periodic knowledge extraction from conversation history.

### Trigger conditions

- Every `synthesis_interval` user messages (default: 5).
- Forced before history compaction (to avoid information loss).
- Forced at the end of soul hatching.

### Process

1. Collect the last 10 user + assistant messages from history.
2. Strip `[Memory context]` blocks from user messages.
3. Call the provider with `build_synthesis_prompt()` at low temperature (0.3).
4. Parse JSON response containing atomic notes with suggested keys, content, and links.
5. Store each note and create links to existing entries.

## Response cache

`ResponseCache` (`src/memory/response_cache.hpp`) deduplicates identical LLM calls.

### Key computation

FNV-1a 64-bit hash over the concatenation of `model`, `system_prompt`, and `user_message`, with separator bytes between fields.

### Eviction strategy

Two-phase eviction on `put()`:

1. **TTL pass:** Remove entries where `now - timestamp > ttl_seconds`.
2. **LRU pass:** If still over `max_entries`, use `nth_element` (O(n)) to partition by `last_access` and remove the oldest.

### Storage

JSON array at `~/.ptrclaw/response_cache.json`. Loaded on construction, saved after every `put()`. Thread-safe via `std::mutex`.

## Session management

`SessionManager` (`src/session.hpp`) handles multi-user sessions for channel-based interactions (Telegram, WhatsApp).

### Session lifecycle

```
Message arrives (session_id = chat ID)
    │
    ├─ Session exists? → update last_active, return Agent&
    │
    └─ Create new session:
         ├─ Resolve provider + model
         ├─ Create builtin tools
         ├─ Construct Agent with memory, cache, event bus
         └─ Publish SessionCreatedEvent
```

### Idle eviction

`evict_idle(max_idle_seconds)` iterates all sessions and removes those where `now - last_active > threshold` (default: 1 hour). Publishes `SessionEvictedEvent` before removal.

### Thread safety

All public methods use `std::lock_guard<std::mutex>`. Each session owns its Agent — no cross-session synchronization needed.

## History compaction

When conversation history grows too large, `compact_history()` trims it while preserving context.

### Trigger conditions

Either of:
- History size exceeds `max_history_messages` (default: 50).
- Estimated token count exceeds 75% of `token_limit` (default: 128,000 × 0.75 = 96,000).

AND history must have more than 12 messages (minimum preserved).

### Algorithm

1. **Force synthesis** — extract knowledge before discarding messages.
2. **Keep system prompt** — position 0, preserved as-is.
3. **Summarize middle** — replace discarded messages with a summary: `[Conversation history compacted. Previous discussion covered: X user messages, Y assistant responses, Z tool calls]`.
4. **Keep last 10** — preserves recent context. If cut point lands on a Tool message, walks back to keep tool call + response pairs intact.
5. **Run hygiene purge** — delete Conversation memory entries older than `hygiene_max_age`.

### Token estimation

- Uses provider-reported `prompt_tokens` when available.
- Falls back to word count × 1.3 heuristic.

## Configuration reference

All memory settings live under the `memory` key in `~/.ptrclaw/config.json`:

```json
{
    "memory": {
        "backend": "sqlite",
        "path": "~/.ptrclaw/memory.db",
        "auto_save": false,
        "recall_limit": 5,
        "hygiene_max_age": 604800,
        "response_cache": false,
        "cache_ttl": 3600,
        "cache_max_entries": 100,
        "enrich_depth": 1,
        "synthesis": true,
        "synthesis_interval": 5,
        "knowledge_max_idle_days": 30,
        "knowledge_survival_chance": 0.05
    }
}
```

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `backend` | string | `"sqlite"` or `"json"` | Storage backend. `"sqlite"` if compiled with SQLite support, else `"json"`. |
| `path` | string | `""` (auto) | Custom path to storage file. Empty uses `~/.ptrclaw/memory.json` or `memory.db`. |
| `auto_save` | bool | `false` | Store every user + assistant message as Conversation entries. |
| `recall_limit` | uint32 | `5` | Max entries recalled per user message for context enrichment. |
| `hygiene_max_age` | uint32 | `604800` | Purge Conversation entries older than this (seconds). 7 days. |
| `response_cache` | bool | `false` | Enable LLM response deduplication cache. |
| `cache_ttl` | uint32 | `3600` | Cache entry time-to-live (seconds). |
| `cache_max_entries` | uint32 | `100` | Max cache entries before LRU eviction. |
| `enrich_depth` | uint32 | `1` | Link-following depth. `0` = flat recall, `1` = include 1-hop neighbors. |
| `synthesis` | bool | `true` | Auto-extract atomic notes from conversation. |
| `synthesis_interval` | uint32 | `5` | Synthesize every N user messages. |
| `recency_half_life` | uint32 | `0` | Recency decay half-life in seconds. `0` = disabled. See [Recency decay](#recency-decay). |
| `knowledge_max_idle_days` | uint32 | `30` | Days of inactivity before a Knowledge entry is eligible for purge. `0` = disabled. See [Knowledge decay](#knowledge-decay). |
| `knowledge_survival_chance` | double | `0.05` | Probability [0.0, 1.0] that an eligible Knowledge entry randomly survives purge. |
| `embeddings.provider` | string | `""` | Embedding provider: `"openai"`, `"ollama"`, or `""` (disabled). |
| `embeddings.model` | string | `""` | Model name. Empty uses provider default (`text-embedding-3-small` / `nomic-embed-text`). |
| `embeddings.base_url` | string | `""` | Override API base URL. Empty uses provider default. |
| `embeddings.api_key` | string | `""` | API key for OpenAI embeddings. Empty falls back to `providers.openai.api_key`. |
| `embeddings.text_weight` | double | `0.4` | Weight for text score in hybrid search. |
| `embeddings.vector_weight` | double | `0.6` | Weight for vector similarity in hybrid search. |

## Recency decay

Optional time-based scoring that favors recent entries over old ones. When enabled, recall scores are multiplied by an exponential decay factor based on entry age.

### Formula

```
score *= exp(-ln(2) × age_seconds / half_life_seconds)
```

| Age (relative to half-life) | Decay multiplier |
|-----------------------------|------------------|
| 0 (just stored)             | 1.0              |
| 1× half-life                | 0.5              |
| 2× half-life                | 0.25             |
| 3× half-life                | 0.125            |

### Configuration

```json
{
    "memory": {
        "recency_half_life": 259200
    }
}
```

The value is in seconds. `0` (default) disables decay entirely. Common values:

| Value | Half-life | Use case |
|-------|-----------|----------|
| `0` | Disabled | All entries scored equally regardless of age |
| `86400` | 1 day | Fast-moving conversations, recent context dominates |
| `259200` | 3 days | Balanced — older knowledge fades but doesn't vanish |
| `604800` | 7 days | Gentle decay, matches default `hygiene_max_age` |

Environment variable: `MEMORY_RECENCY_HALF_LIFE`.

### Interaction with hybrid search

Decay applies **after** hybrid scoring (text + vector), so it multiplies the combined score. This means a semantically relevant old entry can still rank above an irrelevant new one — decay is a tiebreaker, not an override.

Decay applies to both text-only and hybrid search paths in both backends.

## Knowledge decay

Use-it-or-lose-it memory for Knowledge entries. Entries that aren't recalled gradually get purged, mimicking natural memory decay. With a twist: some old memories randomly survive each purge round, just like how humans sometimes remember oddly specific old things.

### How it works

1. Every Knowledge entry has a `last_accessed` timestamp, updated on `store()` and `recall()`.
2. During `hygiene_purge()`, Knowledge entries idle longer than `knowledge_max_idle_days` are eligible for purge.
3. Each eligible entry rolls a random survival chance (`knowledge_survival_chance`).
4. Losers are deleted (along with their links). Survivors get `last_accessed` refreshed to now, so they persist until the next hygiene round.
5. Core entries are never touched. Conversation entries are purged by their own `hygiene_max_age` rule.

### Backwards compatibility

- `last_accessed` defaults to `0`. When `0`, the decay check falls back to `timestamp`, so pre-existing entries without `last_accessed` still decay based on when they were created/updated.
- The field is serialized only when non-zero (JSON) or as a nullable column (SQLite), so old files/databases work unchanged.

### Configuration

```json
{
    "memory": {
        "knowledge_max_idle_days": 30,
        "knowledge_survival_chance": 0.05
    }
}
```

| Value | Effect |
|-------|--------|
| `knowledge_max_idle_days: 0` | Disabled (default: 30). No Knowledge decay. |
| `knowledge_max_idle_days: 7` | Aggressive — unused Knowledge purged after 1 week. |
| `knowledge_max_idle_days: 90` | Gentle — 3 months of inactivity before purge eligibility. |
| `knowledge_survival_chance: 0.0` | No random survival. All idle entries are purged deterministically. |
| `knowledge_survival_chance: 0.05` | 5% chance (default). ~1 in 20 idle entries survives each round. |
| `knowledge_survival_chance: 1.0` | All entries survive (effectively disables decay). |

## Embedding / vector search

Optional semantic search via embedding vectors. When enabled, memory recall combines text matching with cosine similarity for hybrid scoring — finding entries by meaning, not just exact keywords.

### How it works

1. **On store**: Content is sent to the embedding provider to get a vector. Stored alongside the entry (BLOB in SQLite, JSON array in JSON backend).
2. **On recall**: Query is embedded. For each entry, hybrid score = `text_weight × normalized_text_score + vector_weight × normalized_vector_score`.
3. **Backwards compatible**: Entries without embeddings get vector score 0. No migration needed — embeddings accumulate as entries are stored/updated.

### Score normalization

- **Text scores**: Max-normalized to [0, 1] within the result set.
- **Vector scores**: Cosine similarity ([-1, 1]) shifted to [0, 1] via `(sim + 1) / 2`.
- **Combined**: `text_weight × text_norm + vector_weight × vec_norm`.

### Embedding providers

**OpenAI** (`text-embedding-3-small`):
- Requires API key (uses `providers.openai.api_key` as fallback).
- 1536 dimensions.
- `POST {base_url}/embeddings` with `{"model": "...", "input": "text"}`.

**Ollama** (`nomic-embed-text`):
- No API key needed (local).
- 768 dimensions.
- `POST {base_url}/api/embed` with `{"model": "...", "input": "text"}`.

### Configuration example

```json
{
    "memory": {
        "embeddings": {
            "provider": "openai",
            "model": "text-embedding-3-small",
            "text_weight": 0.4,
            "vector_weight": 0.6
        }
    }
}
```

Environment variables: `EMBEDDING_PROVIDER`, `EMBEDDING_MODEL`, `EMBEDDING_API_KEY`.

**Auto-detection:** When no `embeddings.provider` is configured but an OpenAI API key is available (via `providers.openai.api_key` or `OPENAI_API_KEY`), embeddings are automatically enabled using OpenAI. This means if you already have an OpenAI key configured for any reason, embeddings "just work" when compiled with `with_embeddings=true`.

### Thread safety

Embedding HTTP calls happen **outside** the mutex lock (200–500ms network calls). Only the in-memory/DB write is mutex-protected. This prevents embedding latency from blocking other memory operations.

### Snapshots

Embeddings are **not** included in `snapshot_export()` / `snapshot_import()`. This keeps the format portable and small. Embeddings are regenerated as entries are stored/updated with an active embedder.

### JSON file format

Backwards-compatible detection:
- **Array** (legacy): `[{entry}, ...]` — no embeddings.
- **Object** (new): `{"entries": [...], "embeddings": {"key": [floats...]}}` — written only when embeddings exist.

## Build flags

Feature flags in `meson_options.txt` control what gets compiled:

| Flag | Default | Controls |
|------|---------|----------|
| `with_memory` | `true` | Memory system (JSON + None backends, enrichment, synthesis). |
| `with_sqlite_memory` | `true` | SQLite backend with FTS5. Requires `sqlite3` dependency. |
| `with_memory_tools` | `true` | The four LLM-accessible memory tools. |
| `with_embeddings` | `true` | Embedding/vector search. Requires `with_memory`. |

```bash
# Build without SQLite (JSON backend only)
meson setup build -Dwith_sqlite_memory=false

# Build without memory entirely
meson setup build -Dwith_memory=false

# Build with embedding/vector search
meson setup build -Dwith_embeddings=true
```

## Key source files

| File | Purpose |
|------|---------|
| `src/memory.hpp` | Memory interface, MemoryEntry, MemoryCategory, enrichment helpers |
| `src/memory.cpp` | Factory, `memory_enrich()`, `collect_neighbors()` |
| `src/memory/entry_json.hpp` | Shared `entry_to_json()` / `entry_from_json()` used by both backends |
| `src/memory/json_memory.hpp/.cpp` | JSON file backend |
| `src/memory/sqlite_memory.hpp/.cpp` | SQLite + FTS5 backend |
| `src/memory/none_memory.hpp/.cpp` | No-op backend |
| `src/memory/response_cache.hpp/.cpp` | LLM response cache |
| `src/embedder.hpp` | Embedder interface, `Embedding` type, `cosine_similarity()`, `recency_decay()` |
| `src/embedder.cpp` | `create_embedder()` factory |
| `src/embedders/http_embedder.hpp/.cpp` | Unified HTTP embedder (OpenAI, Ollama) |
| `src/tools/memory_tool_util.hpp` | Shared `parse_memory_tool_args()` / `require_string()` for memory tools |
| `src/tools/memory_store.hpp/.cpp` | memory_store tool |
| `src/tools/memory_recall.hpp/.cpp` | memory_recall tool |
| `src/tools/memory_forget.hpp/.cpp` | memory_forget tool |
| `src/tools/memory_link.hpp/.cpp` | memory_link tool |
| `src/agent.hpp/.cpp` | Agent loop, synthesis, compaction, enrichment integration |
| `src/prompt.hpp/.cpp` | System prompt building, soul hatching, synthesis prompts |
| `src/session.hpp/.cpp` | Multi-session management, idle eviction |
| `src/config.hpp` | MemoryConfig struct |
| `src/plugin.hpp` | Plugin registry, MemoryRegistrar |
