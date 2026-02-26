# PtrClaw

An AI assistant you can actually deploy anywhere. Single static binary, no runtime dependencies, no containers needed. Run it as a CLI tool, a Telegram bot, or plug in your own channel. WhatsApp is available as an opt-in build flag.

Built in C++17 because infrastructure should be small, fast, and boring to operate.

**~681 KB static binary (macOS arm64), ~6.9 MB (Linux x86_64, statically linked with OpenSSL + sqlite3). 6 LLM providers. 9 built-in tools. Telegram channel (+ WhatsApp opt-in). Persistent memory with knowledge graph. Compile-time feature flags to strip what you don't need.**

## Why PtrClaw?

Most AI agent frameworks are Python packages with deep dependency trees, virtual environments, and careful version management just to get running. PtrClaw is a single binary that you `scp` to a server and run. It compiles in seconds, starts instantly, and uses minimal memory. You *can* containerize it (Docker support coming soon), but you don't *have* to.

- **Deploy anywhere** — one static binary, no runtime deps, runs on any Linux box or Mac
- **Swap providers freely** — Anthropic, OpenAI, OpenRouter, Ollama, or any OpenAI-compatible endpoint. Switch with a config change, no code modifications
- **Real tool use** — file I/O, shell execution (with stdin piping), cron scheduling, and a persistent knowledge graph memory system. Providers with native function calling use it directly; others fall back to XML-based parsing
- **Extend without forking** — providers, channels, tools, and memory backends self-register via a plugin system. Add a new one by implementing an interface and dropping in a `.cpp` file
- **Build only what you need** — 12 compile-time feature flags let you strip unused providers, channels, and tools for smaller binaries (down to ~599 KB)

## Features

- **LLM streaming** — real-time token streaming with progressive message editing in channels
- **Event-driven architecture** — publish/subscribe event bus decouples agent, channels, and streaming
- **Interactive REPL** with slash commands (`/status`, `/model`, `/models`, `/provider`, `/clear`, `/memory`, `/auth`, `/onboard`, `/hatch`, `/help`, `/quit`; `/soul` available in `--dev` mode)
- **Single-message mode** — pipe a question in and get an answer back
- **Automatic history compaction** when token usage approaches the context limit
- **Persistent memory** — knowledge graph with bidirectional links, three-space semantics (core/knowledge/conversation), graph-aware context enrichment, automatic conversation synthesis
- **Provider failover** — `reliable` provider wraps multiple backends with automatic fallback
- **Cron scheduling** — the agent can schedule recurring tasks via system crontab and send results back via `--notify`
- **Multi-session management** with idle eviction
- **Telegram channel** — long-polling, user allowlists, Markdown-to-HTML, per-user sessions, streaming message edits
- **WhatsApp channel** *(opt-in: `-Dwith_whatsapp=true`)* — Business Cloud API with built-in webhook server (reverse-proxy ready), E.164 phone normalization, sender allowlists
- **Soul hatching** — dynamic personality development through onboarding conversations

## Quick start

```sh
git clone https://github.com/heikki-laitala/ptrclaw.git && cd ptrclaw
make deps    # install build dependencies
make build   # compile (output: builddir/ptrclaw)
./builddir/ptrclaw
```

On first run, PtrClaw detects that no provider credentials are configured and launches an interactive setup wizard. It walks you through choosing a provider, entering credentials (API key or OAuth), optionally setting up a messaging channel, and creating a personality for your assistant.

You can re-run the wizard at any time with `/onboard`, or set credentials individually with `/auth <provider>`.

If you prefer manual setup, set an environment variable or create a config file (see [Configuration](#configuration)):

```sh
export ANTHROPIC_API_KEY=sk-ant-...
./builddir/ptrclaw
```

No virtual environments, no package managers at runtime.

## Requirements

- C++17 compiler (Clang 15+ recommended, GCC 10+ works)
- [Meson](https://mesonbuild.com/) + Ninja
- libssl (OpenSSL)
- libcurl (macOS only; Linux uses a built-in socket HTTP backend)
- libsqlite3 (optional — for SQLite memory backend; the default JSON file backend has zero dependencies)

### macOS

```sh
brew install meson llvm gcovr sqlite3
```

### Linux (Debian/Ubuntu)

```sh
sudo apt-get install g++ meson ninja-build libssl-dev libsqlite3-dev clang-tidy lld gcovr
```

Linux builds use `clang++` with the `lld` linker (required for LTO) via `meson-native-linux.ini`. If `clang++` is not available, the build falls back to the default system compiler.

Or run `make deps` to install everything automatically.

## Configuration

Create `~/.ptrclaw/config.json`:

```json
{
  "provider": "anthropic",
  "model": "claude-sonnet-4-6",
  "temperature": 0.7,
  "base_url": "",
  "providers": {
    "anthropic": { "api_key": "sk-ant-..." },
    "openai": { "api_key": "sk-..." },
    "openrouter": { "api_key": "sk-or-..." },
    "ollama": { "base_url": "http://localhost:11434" },
    "compatible": {
      "api_key": "optional-if-required-by-endpoint",
      "base_url": "http://localhost:8080/v1"
    }
  },
  "agent": {
    "max_tool_iterations": 10,
    "max_history_messages": 50,
    "token_limit": 128000,
    "disable_streaming": false
  },
  "memory": {
    "backend": "sqlite",
    "path": "~/.ptrclaw/memory.db",
    "enrich_depth": 1,
    "synthesis": true,
    "synthesis_interval": 5
  },
  "channels": {
    "telegram": {
      "bot_token": "123456:ABC-DEF...",
      "allow_from": ["alice", "bob"],
      "reply_in_private": true,
      "proxy": ""
    },
    "whatsapp": {
      "access_token": "EAA...",
      "phone_number_id": "123456789",
      "verify_token": "my-verify-secret",
      "app_secret": "",
      "allow_from": ["+1234567890"],
      "webhook_listen": "127.0.0.1:8080",
      "webhook_secret": "a-strong-random-secret",
      "webhook_max_body": 65536
    }
  }
}
```

Minimal config (Anthropic only):

```json
{
  "provider": "anthropic",
  "model": "claude-sonnet-4-6",
  "providers": {
    "anthropic": {
      "api_key": "sk-ant-...",
      "prompt_caching": true
    }
  }
}
```

Environment variables override the config file:

| Variable | Description |
| --- | --- |
| `ANTHROPIC_API_KEY` | Anthropic API key |
| `OPENAI_API_KEY` | OpenAI API key |
| `OPENAI_USE_OAUTH` | Use OpenAI subscription OAuth token path (`true`/`1`) |
| `OPENAI_OAUTH_ACCESS_TOKEN` | OpenAI OAuth access token |
| `OPENAI_OAUTH_REFRESH_TOKEN` | OpenAI OAuth refresh token |
| `OPENAI_OAUTH_EXPIRES_AT` | Access token expiry (epoch seconds) |
| `OPENAI_OAUTH_CLIENT_ID` | OAuth client id (default `app_EMoamEEZ73f0CkXaXp7hrann`) |
| `OPENAI_OAUTH_TOKEN_URL` | OAuth token endpoint (default `https://auth.openai.com/oauth/token`) |
| `OPENROUTER_API_KEY` | OpenRouter API key |
| `OLLAMA_BASE_URL` | Ollama server URL (default `http://localhost:11434`) |
| `COMPATIBLE_BASE_URL` | Base URL for OpenAI-compatible endpoint |
| `BASE_URL` | Global base URL override for the active provider |
| `TELEGRAM_BOT_TOKEN` | Telegram bot token (overrides config) |
| `WHATSAPP_ACCESS_TOKEN` | WhatsApp Business API access token |
| `WHATSAPP_PHONE_ID` | WhatsApp Business phone number ID |
| `WHATSAPP_VERIFY_TOKEN` | WhatsApp webhook verification token |
| `WHATSAPP_WEBHOOK_LISTEN` | Bind address for built-in webhook server (e.g. `127.0.0.1:8080`) |
| `WHATSAPP_WEBHOOK_SECRET` | Shared secret for proxy→local trust (`X-Webhook-Secret` header) |

### Setting credentials with `/auth`

```text
/auth                    Show auth status for all providers
/auth <provider>         Set credentials interactively
```

```text
ptrclaw> /auth
Auth status:
  Anthropic (Claude)         not configured
  OpenAI (GPT)               not configured
  OpenRouter (multi-model)   not configured
  Ollama (local)             http://localhost:11434

Set credentials: /auth <provider>

ptrclaw> /auth anthropic
Enter your Anthropic (Claude) API key: sk-ant-...
Saved.
```

For OpenAI, `/auth openai` offers a choice between API key and OAuth login:

```text
ptrclaw> /auth openai
Authentication method:
  1. API key
  2. OAuth login (ChatGPT subscription)
> 2

Open this URL to authorize:
https://auth.openai.com/oauth/authorize?...

Paste the callback URL or code: http://localhost:1455/auth/callback?code=...
OAuth connected. Model switched to gpt-5-codex-mini.
```

For Ollama, `/auth ollama` prompts for the base URL.

In Telegram and other channels, use `/auth <provider> <api_key>` to set a key directly. OAuth uses the two-step flow: `/auth openai start` then `/auth openai finish <callback_url>`.

### OpenAI OAuth (codex models)

OpenAI codex models (e.g. `gpt-5-codex-mini`) can be accessed via OAuth using your ChatGPT subscription (Plus, Pro, or Team). Use `/auth openai` and choose "OAuth login". PtrClaw automatically prefers OAuth for codex models when tokens are available, falling back to the API key otherwise. Non-codex models always use the API key.

For details on the PKCE flow, token refresh, environment variables, and config format, see [`docs/openai-oauth.md`](docs/openai-oauth.md).

### Switching providers and models

```text
/models                              # list configured providers and current model
/provider openai gpt-4o-mini         # switch to OpenAI API key
/provider openai gpt-5-codex-mini    # switch to OpenAI OAuth (codex)
/provider anthropic claude-sonnet-4-6  # switch to Anthropic
/model gpt-4o                        # switch model within current provider
```

Provider and model changes are persisted to `~/.ptrclaw/config.json`.

### Configuration notes

- There is currently no `COMPATIBLE_API_KEY` env var; set `providers.compatible.api_key` in `~/.ptrclaw/config.json` when using the `compatible` provider.
- `BASE_URL` overrides provider-specific base URLs for whichever provider is active.
- `providers.anthropic.prompt_caching` controls Anthropic provider-side prompt caching (default: `true`). Set it to `false` to disable.
- OpenAI OAuth tokens are refreshed automatically when they expire (requires `oauth_refresh_token`).
- The `use_oauth` field in config is managed automatically — you don't need to set it manually.

### How to get a Telegram bot token

1. Open Telegram and start a chat with [@BotFather](https://t.me/BotFather).
2. Send `/newbot` and follow the prompts:
   - Bot display name (can contain spaces)
   - Bot username (must end with `bot`, e.g. `ptrclaw_helper_bot`)
3. BotFather returns an HTTP API token in this format:
   - `123456789:AA...`
4. Add it to config or env:

```sh
export TELEGRAM_BOT_TOKEN="123456789:AA..."
```

Optional hardening:
- In BotFather, run `/setprivacy` for your bot.
  - **Enable** privacy mode for group chats where you only want commands/mentions.
  - **Disable** privacy mode if your bot needs to read all group messages.

### WhatsApp setup

WhatsApp is not included in the default build — enable it with `-Dwith_whatsapp=true`. It requires Meta Cloud API credentials and a webhook server behind a reverse proxy.

See [`docs/whatsapp.md`](docs/whatsapp.md) for credentials setup and [`docs/reverse-proxy.md`](docs/reverse-proxy.md) for proxy configuration.

## Usage

### Interactive REPL

```sh
./builddir/ptrclaw
```

### Single message

```sh
./builddir/ptrclaw -m "Explain what src/agent.cpp does"
```

### Telegram bot

```sh
export TELEGRAM_BOT_TOKEN=123456:ABC-DEF...
./builddir/ptrclaw --channel telegram
```

### CLI options

```text
-m, --message MSG        Send a single message and exit
--notify CHAN:TARGET      After -m, send response via channel (e.g. telegram:123456)
--channel NAME           Run as a channel bot (telegram, whatsapp¹)
--provider NAME          Use a specific provider (anthropic, openai, ollama, openrouter)
--model NAME             Use a specific model
--dev                    Enable developer-only commands (e.g. /soul)
-h, --help               Show help

¹ WhatsApp requires building with -Dwith_whatsapp=true
```

## Development

```sh
make build          # compile (all features except WhatsApp)
make build-minimal  # slim build: openai + telegram + tools + json memory
make build-static   # size-optimized static binary for distribution
make test           # run unit tests (Catch2)
make lint           # run clang-tidy
make coverage       # generate HTML coverage report
make clean          # remove build artifacts
```

Distribution builds (`build-static`, `build-minimal`) compile only the main binary with size optimizations. On Linux: `-ffunction-sections`, `-fdata-sections`, `-fvisibility=hidden`, `--gc-sections`, `--strip-all`, plus `strip --strip-unneeded`. On macOS: `strip -x` (Apple's linker handles dead stripping with LTO).

### Feature flags

Every provider, channel, and tool is a compile-time feature flag in `meson_options.txt`. Most default to `true`; WhatsApp defaults to `false` (requires webhook infrastructure). Disable unused components to reduce binary size:

| Flag | Controls | Default |
| ---- | -------- | ------- |
| `with_anthropic` | Anthropic provider | `true` |
| `with_openai` | OpenAI provider + SSE parser | `true` |
| `with_openrouter` | OpenRouter provider (implies `with_openai`) | `true` |
| `with_compatible` | OpenAI-compatible provider (implies `with_openai`) | `true` |
| `with_ollama` | Ollama provider | `true` |
| `with_telegram` | Telegram channel | `true` |
| `with_whatsapp` | WhatsApp channel | `false` |
| `with_pipe` | Pipe channel (JSONL stdin/stdout) | `false` |
| `with_tools` | All built-in tools | `true` |
| `with_memory` | Memory system (JsonMemory) | `true` |
| `with_sqlite_memory` | SQLite+FTS5 memory backend | `true` |
| `with_memory_tools` | Memory tools (store, recall, forget, link) | `true` |

Pass `-D` flags to `meson setup`:

```sh
# Anthropic-only CLI (no channels, no tools)
meson setup build_slim -Dwith_openai=false -Dwith_ollama=false \
  -Dwith_telegram=false -Dwith_whatsapp=false -Dwith_tools=false

# Local-only with Ollama
meson setup build_local -Dwith_anthropic=false -Dwith_openai=false \
  -Dwith_telegram=false -Dwith_whatsapp=false

ninja -C build_slim
```

Reconfigure an existing build dir:

```sh
meson configure builddir -Dwith_whatsapp=false
ninja -C builddir
```

### Binary size

| Configuration | macOS arm64 | Linux x86_64 |
| ------------- | ----------- | ------------ |
| Default (`make build`) | ~916 KB | ~916 KB |
| Static (`make build-static`, stripped) | ~681 KB | ~6.9 MB |
| Minimal (`make build-minimal`, stripped) | ~599 KB | ~650 KB |

Default builds exclude WhatsApp (enable with `-Dwith_whatsapp=true`). Linux static binaries are larger because they bundle OpenSSL and sqlite3. The dynamically linked build is the same size as macOS. LTO is enabled by default. Distribution builds are stripped and size-optimized.

## Project structure

```text
src/
  main.cpp              CLI entry point, REPL, and slash command handling
  agent.hpp/cpp         Agentic loop — chat, tool dispatch, history compaction
  onboard.hpp/cpp       First-run setup wizard (provider, channel, personality)
  provider.hpp/cpp      Provider interface, types, listing, and runtime switching
  tool.hpp/cpp          Tool interface, ToolSpec, ToolResult
  channel.hpp/cpp       Channel interface, ChannelMessage
  config.hpp/cpp        Config loading (~/.ptrclaw/config.json + env vars)
  plugin.hpp/cpp        Self-registration plugin registry (providers, channels, tools)
  event.hpp             Event types for the publish/subscribe bus
  event_bus.hpp/cpp     Thread-safe publish/subscribe event bus
  stream_relay.hpp/cpp  Bridges stream events to progressive channel message editing
  dispatcher.hpp/cpp    XML tool-call parsing for non-native providers
  session.hpp/cpp       Multi-session management with idle eviction
  oauth.hpp/cpp         OpenAI OAuth PKCE flow, token exchange, and refresh wiring
  prompt.hpp/cpp        System prompt builder
  http.hpp              HttpClient interface
  http_socket.cpp       Linux HTTP backend (POSIX sockets + OpenSSL)
  http.cpp              macOS HTTP backend (libcurl)
  util.hpp/cpp          String/path utilities
  channels/
    telegram.hpp/cpp    Telegram Bot API (long-polling, Markdown→HTML, streaming edits)
    whatsapp.hpp/cpp    WhatsApp Business Cloud API (webhook server, message parsing)
    webhook_server.hpp/cpp  Minimal HTTP server for receiving webhooks behind a reverse proxy
  providers/
    anthropic.cpp       Anthropic Messages API (streaming)
    openai.cpp          OpenAI Chat Completions + Responses API (streaming, OAuth)
    openrouter.cpp      OpenRouter (inherits OpenAI)
    ollama.cpp          Ollama local inference
    compatible.cpp      Generic OpenAI-compatible endpoint
    reliable.cpp        Failover wrapper over multiple providers
    sse.cpp             Server-Sent Events parser
  memory/
    json_memory.cpp     JSON file backend with knowledge graph links
    sqlite_memory.cpp   SQLite+FTS5 backend (optional)
    none_memory.cpp     No-op backend
    response_cache.cpp  LLM response deduplication cache
  tools/
    file_read.cpp       Read file contents
    file_write.cpp      Write/create files
    file_edit.cpp       Search-and-replace edits
    shell.cpp           Shell command execution (with stdin support)
    cron.cpp            Cron scheduling (list, add, remove system crontab entries)
    memory_store.cpp    Store/upsert memory entries with optional links
    memory_recall.cpp   Search memories with graph traversal
    memory_forget.cpp   Delete memory entries
    memory_link.cpp     Create/remove bidirectional links between entries
tests/                  Catch2 unit tests
docs/
  openai-oauth.md       OpenAI OAuth PKCE flow, token refresh, and config format
  reverse-proxy.md      Reverse-proxy setup for WhatsApp webhooks (nginx, Caddy)
  whatsapp.md           WhatsApp Cloud API credentials and webhook setup
meson_options.txt       Compile-time feature flags
```

## Acknowledgements

PtrClaw started as a C++ port of [nullclaw](https://github.com/nullclaw/nullclaw) and has since diverged into its own architecture with an event bus, plugin system, and streaming pipeline. Nullclaw is a far more feature-rich project — if you need a battle-tested assistant with a broader tool ecosystem, check it out. PtrClaw trades breadth for a smaller footprint: our dynamically linked binary is ~916 KB vs nullclaw's ~1.9 MB, with the goal of staying minimal and easy to embed or deploy on constrained environments.
