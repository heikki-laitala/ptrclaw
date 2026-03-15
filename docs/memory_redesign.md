# Memory redesign

This document describes how the `feat/memory-redesign` branch changes PtrClaw memory behavior beyond the baseline in `main`.

It focuses on the implemented redesign as it exists now, not just the original idea.

## Goals

The redesign aims to improve four things at once:

- preserve long conversation history better
- keep prompt-visible context bounded
- distinguish durable concepts from session-specific observations
- make memory retrieval more intentional for different kinds of user questions

The overall direction combines two ideas:

- **lossless-ish history preservation**: compact old history without turning it into total amnesia
- **episode -> concept consolidation**: convert concrete experiences into reusable durable memory

## High-level model

The redesigned system has four practical layers.

### 1. Identity / core memory

This is still the stable always-on layer:

- soul / personality
- user identity facts
- durable behavioral guidance

This is represented through `Core` memory and the system prompt.

### 2. Episode memory

This is concrete experience memory.

It includes:

- recent raw conversation tail
- compacted episode summaries inserted into active history
- archived episode records containing the full dropped message slice

This layer is used when chronology or prior session events matter.

### 3. Concept memory

This is generalized durable memory.

It includes:

- preferences
n- decisions
- constraints
- recurring patterns
- stable project facts

This layer is used when the user is asking about stable facts, preferences, or cross-session patterns.

### 4. Context assembly

Before each user message is sent to the model, PtrClaw now tries to assemble context more intentionally.

Instead of treating all recalled memory as one flat pool, it distinguishes between:

- concepts
- observations
- past episodes

and biases what is shown based on query intent.

## What changed from the old design

## Old behavior

Baseline `main` roughly did this:

- recall memory entries
- prepend a flat `[Memory context]` block
- on compaction, replace old history with one generic summary line
- keep only the last raw tail
- rely on synthesis to salvage some information into notes

That worked, but it was fairly coarse.

## New behavior

The redesign adds:

- structured episode summaries instead of a generic compaction message
- recoverable archived episode slices
- durable persistence of archived episodes in JSON/SQLite backends
- synthesis that distinguishes `concept` vs `observation`
- concept replacement / contradiction handling
- intent-aware retrieval/ranking between concepts and observations

## Compaction and episode summaries

When history grows too large, `Agent::compact_history()` still compacts it, but now it does more than emit a generic marker.

### Structured summary

PtrClaw builds an `EpisodeSummary` that tracks things like:

- user turn count
- assistant turn count
- tool call count
- tools used in the discarded slice

That summary is inserted into active history as a user-visible compacted history marker.

### Archived episode records

The discarded slice is also turned into an `EpisodeRecord` containing:

- stable episode id like `episode:0`
- timestamp
- user/assistant/tool counts
- tool names
- full archived `ChatMessage` slice

So compaction becomes:

- keep recent raw tail in history
- insert structured summary
- archive the actual dropped slice for later recovery

## Episode archive persistence

Initially archived episodes were only kept in agent memory. That is no longer the case.

### Memory interface additions

The memory interface now supports:

- `save_episode_archive(...)`
- `load_episode_archive()`

These are intentionally separate from normal knowledge recall APIs.

### JSON backend

`JsonMemory` persists episode archive data under a top-level `episode_archive` field.

### SQLite backend

`SqliteMemory` persists episode archive data in a dedicated `episode_archive` table.

### Agent restore path

When memory is attached/restored, the agent now reloads archived episodes and restores the internal episode counter so new ids do not collide with old ones.

This means episode memory is now durable across restarts, not just during one process lifetime.

## Synthesis: episode -> concept consolidation

The redesign changes synthesis from “extract notes” toward a more explicit distinction between durable and session-specific memory.

### Synthesis prompt contract

The synthesis prompt now tells the model to classify output notes as:

- **concept**
  - stable and generalizable
  - preferences, decisions, constraints, patterns

- **observation**
  - episode-specific/session-specific
  - what happened in this conversation

### Storage behavior

When synthesis output is parsed:

- **concepts** are stored **without `session_id`**
- **observations** are stored **with the current `session_id`**
- `Core` entries are always treated as durable concept-like entries

This makes concept memory naturally cross-session while keeping observations episode-scoped.

## Concept refinement and contradiction handling

The redesign also improves what happens when synthesized concepts change over time.

### Deduplication by key reuse

The synthesis prompt now explicitly instructs the model:

- reuse the same key when refining the same concept

This means “update existing concept” instead of blindly creating duplicates.

### Replacement / contradiction handling

A synthesized note can now optionally include:

- `replaces: "old-key"`

When that happens, PtrClaw will:

- store/update the new concept
- migrate graph links from the old concept to the new one
- remove the outdated concept

This gives a practical first version of contradiction handling and concept evolution.

It is still lightweight, but it prevents stale contradicted concepts from piling up forever.

## Layered memory context

`memory_enrich()` no longer only produces a flat block of undifferentiated recalled notes.

It now organizes output into sections like:

- `Concepts:`
- `Observations:`
- `Past episodes: ...`

This gives the model a better signal about the role of each recalled item.

### Why this matters

- concepts answer stable preference/fact questions
- observations answer session-specific questions
- past episodes signal where compacted chronological history exists

So the context block is more structured and role-aware.

## Retrieval and ranking policy

The redesign adds a first explicit retrieval policy instead of treating all recalled memory identically.

### Query intent classification

PtrClaw now classifies the user message into one of:

- `Stable`
- `Chronological`
- `Unknown`

using lightweight keyword heuristics.

Examples:

- chronological queries: “recently”, “earlier”, “what happened”, “history”
- stable queries: “prefer”, “favorite”, “usually”, “always”

### Tier budgets

Recall budget is now split across concepts and observations depending on query intent.

- **Chronological** query
  - more budget goes to observations
- **Stable** query
  - more budget goes to concepts
- **Unknown** query
  - more even split

### In-tier ranking

Within each tier, entries are sorted by descending score.

That means:

- better matching concepts appear earlier than weaker concepts
- better matching observations appear earlier than weaker observations
- one noisy tier is less likely to crowd out the other entirely

This is still a first-pass heuristic policy, but it is much more intentional than before.

## What is implemented now

As of this branch, the redesign includes:

- structured episode summaries
- recoverable archived episodes
- durable JSON/SQLite episode archive persistence
- concept vs observation synthesis classification
- concept replacement / contradiction handling via `replaces`
- layered memory context sections
- query-intent-based retrieval/ranking policy

## What is still intentionally lightweight

This redesign does **not** yet try to be a giant memory platform.

It does not add:

- a full DAG summary graph
- semantic/LLM-level retrieval policy engine
- a heavy external benchmark service
- complex concept ontologies

The design stays compact and implementation-oriented.

## Practical file/code areas involved

The redesign mainly touches:

- `src/agent.cpp`
- `src/agent.hpp`
- `src/memory.cpp`
- `src/memory.hpp`
- `src/memory/json_memory.*`
- `src/memory/sqlite_memory.*`
- `src/prompt.cpp`
- tests under `tests/`

## Summary

The redesigned memory system changes PtrClaw from:

- flat recall + generic compaction + note extraction

into something closer to:

- structured episode preservation
- durable archived history
- concept-oriented synthesis
- contradiction-aware concept evolution
- layered, intent-aware context assembly

In short:

- **episodes preserve what happened**
- **concepts preserve what was learned**
- **context assembly decides what the model needs right now**
