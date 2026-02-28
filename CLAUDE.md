# PtrClaw — Agent Instructions

Lightweight, extensible AI assistant infrastructure in C++17. Enables LLM-driven tool use via CLI, Telegram, and WhatsApp. Event-driven architecture with a plugin system for providers, channels, and tools.

Optimized for:

- **Minimal binary size** — target: smallest possible static binary. Avoid unnecessary dependencies and abstractions.
- **Minimal memory footprint** — prefer stack allocation, avoid gratuitous heap use.
- **Minimal external dependencies** — libcurl (macOS) or POSIX sockets+OpenSSL (Linux), sqlite3 (optional). Everything else is header-only or subproject.

## Build & Test

```bash
make build          # Build with meson/ninja (clang++, LTO)
make build-minimal  # Slim build: openai + telegram + tools only
make build-static   # Size-optimized static binary for distribution
make test           # Run Catch2 unit tests
make lint           # Run clang-tidy
make coverage       # Generate coverage report
```

Dependencies: `make deps` (requires Homebrew on macOS, apt on Linux).

Validation before commit:

```bash
make test && make lint   # Both must pass
```

## Architecture

```text
Agent (agentic loop)
├── Provider (LLM API) ── Anthropic, OpenAI, OpenRouter, Ollama, Compatible
├── Tool (actions)     ── file_read, file_write, file_edit, shell, cron, memory_store, memory_recall, memory_forget, memory_link
├── Dispatcher         ── XML tool call parsing for non-native providers
├── Memory (pluggable) ── JsonMemory (file-based, knowledge graph), SqliteMemory (FTS5), NoneMemory (no-op)
│   └── ResponseCache  ── LLM response deduplication (FNV-1a hash, TTL+LRU)
├── Session            ── Multi-user session management with idle eviction
└── Channel (I/O)      ── Telegram (long-polling), WhatsApp (webhooks)
```

All source is in `namespace ptrclaw`. Interfaces are abstract base classes with virtual methods (`Provider`, `Tool`, `Channel`). Extend capabilities by implementing these interfaces and registering in the corresponding factory.

Extension points:

- `src/providers/` — add `<name>.hpp/cpp` implementing `Provider`, self-registers via static `ProviderRegistrar`
- `src/channels/` — add `<name>.hpp/cpp` implementing `Channel`, self-registers via static `ChannelRegistrar`
- `src/tools/` — add `<name>.hpp/cpp` implementing `Tool`, self-registers via static `ToolRegistrar`
- `src/memory/` — add `<name>.hpp/cpp` implementing `Memory`, self-registers via static `MemoryRegistrar`
- `meson_options.txt` — add a `with_<name>` feature flag, gate the source in `meson.build`

## Engineering Principles

### KISS

Prefer straightforward control flow. Keep error paths obvious and localized.

### YAGNI

Do not add interfaces, config keys, or abstractions without a concrete caller. No speculative features.

### DRY (Rule of Three)

Duplicate small local logic when it preserves clarity. Extract shared helpers only after three repeated, stable patterns.

### Secure by Default

Never log secrets or tokens. Validate at system boundaries. Keep network/filesystem/shell scope narrow.

## Conventions

- **C++17**, compiled with clang++ (LTO enabled)
- **Naming**: PascalCase types, snake_case functions, trailing underscore for members (`config_`)
- **Headers**: `#pragma once`, local includes first, then system
- **Memory**: `std::unique_ptr` for polymorphic ownership, `std::optional` for nullable values
- **JSON**: nlohmann/json (`#include <nlohmann/json.hpp>`)
- **Tests**: Catch2 with `TEST_CASE` / `REQUIRE`. Mock classes inherit from abstract interfaces.
- **Git**: Conventional commits (`feat:`, `fix:`, `chore:`, `refactor:`, `test:`, `ci:`). No Co-Authored-By trailer. No "Generated with Claude Code" footer in PR descriptions.

## Key Files

| Path | Purpose |
| ---- | ------- |
| `src/agent.hpp` | Core agentic loop: prompt injection, tool dispatch, history compaction, memory integration |
| `src/provider.hpp` | Provider interface + ChatMessage, ChatResponse, ToolCall types |
| `src/tool.hpp` | Tool interface + ToolSpec, ToolResult types |
| `src/memory.hpp` | Memory interface, MemoryEntry, MemoryCategory, MemoryAwareTool base class |
| `src/memory/json_memory.hpp` | JSON file backend (default, zero deps) |
| `src/memory/sqlite_memory.hpp` | SQLite+FTS5 backend (optional, requires sqlite3) |
| `src/memory/response_cache.hpp` | LLM response cache (FNV-1a hash, TTL+LRU eviction) |
| `src/channel.hpp` | Channel interface + ChannelMessage type |
| `src/config.hpp` | Config loader (~/.ptrclaw/config.json + env vars), MemoryConfig |
| `src/dispatcher.hpp` | XML tool call parsing for non-native providers |
| `src/session.hpp` | Thread-safe multi-session management |
| `meson.build` | Build config: static lib + executable + tests, feature-flag gating |
| `meson_options.txt` | Compile-time feature flags for optional components |
| `.clang-tidy` | Linting rules (bugprone, modernize, performance, readability) |

## Risk Tiers

- **Low**: docs, comments, test additions, formatting
- **Medium**: most `src/` behavior changes without security impact
- **High**: `src/tools/` (shell execution), `src/config.hpp` (secrets), channel auth, session management

When uncertain, classify as higher risk.

## Lint

clang-tidy is configured via `.clang-tidy`. The Makefile filters subproject warnings from output. Use `value_or()` instead of `.value()` on optionals to avoid `bugprone-unchecked-optional-access` in test code.

## Platform Notes

- **Linux**: Uses `clang++` with `lld` linker (required for LTO) via `meson-native-linux.ini`. Falls back to default compiler if `clang++` is not available.
- **macOS**: Uses system `clang++`. Lint needs extra args for stdlib/sysroot (handled by Makefile).
- **Distribution builds**: `build-static` and `build-minimal` compile only the `ptrclaw` binary (no tests) with size optimization flags. Linux: `-ffunction-sections`, `-fdata-sections`, `-fvisibility=hidden`, `--gc-sections`, `--strip-all`. macOS: `strip -x`.

## Anti-Patterns (Do Not)

- Do not add dependencies without strong justification (binary size impact).
- Do not modify unrelated code "while here".
- Do not silently weaken security or access constraints.
- Do not add speculative abstractions "just in case".
- Do not commit real API keys, tokens, or credentials. Use `"test-key"` in tests.
- Do not skip `make test && make lint` before committing.
