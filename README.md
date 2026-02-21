# PtrClaw

A lightweight, extensible AI assistant infrastructure in C++17. Run it as a CLI, a Telegram bot, a WhatsApp bot — or plug in your own channel.

**~514 KB static binary. Single POSIX-socket + OpenSSL HTTP backend on Linux and macOS. 5 providers, 4 tools, 2 messaging channels, and compile-time feature flags.**

## Features

- **Multiple providers** — Anthropic, OpenAI, OpenRouter, Ollama, or any OpenAI-compatible endpoint
- **Native tool use** — providers with function calling use it natively; others fall back to XML-based tool parsing
- **Built-in tools** — `file_read`, `file_write`, `file_edit`, `shell`
- **LLM streaming** — real-time token streaming with progressive message editing in channels
- **Event-driven architecture** — publish/subscribe event bus decouples agent, channels, and streaming
- **Plugin system** — providers, channels, and tools self-register; add new ones without touching core code
- **Compile-time feature flags** — include only the components you need for smaller binaries
- **Interactive REPL** with slash commands (`/status`, `/model`, `/clear`, `/help`, `/quit`)
- **Single-message mode** — pipe a question in and get an answer back
- **Automatic history compaction** when token usage approaches the context limit
- **Provider failover** — `reliable` provider wraps multiple backends with automatic fallback
- **Multi-session management** with idle eviction
- **Telegram channel** — long-polling, user allowlists, Markdown-to-HTML, per-user sessions, streaming message edits
- **WhatsApp channel** — Business Cloud API with webhooks, E.164 phone normalization, sender allowlists

## Quick start

> Dev workflow: pull latest `main`, create a feature branch, push, and open a PR.

```sh
git clone <repo-url> && cd ptrclaw
make deps    # install meson, llvm, ssl deps
make build   # compile (output: builddir/ptrclaw)

export ANTHROPIC_API_KEY=sk-ant-...
./builddir/ptrclaw
```

## Requirements

- C++17 compiler (Clang 15+ recommended, GCC 10+ works)
- [Meson](https://mesonbuild.com/) + Ninja
- OpenSSL (Linux: `libssl-dev`; macOS: Homebrew `openssl@3`)

### macOS

```sh
brew install meson llvm gcovr openssl@3
```

### Linux (Debian/Ubuntu)

```sh
sudo apt-get install g++ meson ninja-build libssl-dev clang-tidy lld gcovr
```

Linux builds use `clang++` with the `lld` linker (required for LTO) via `meson-native-linux.ini`.

Or run `make deps` to install everything automatically.

## Configuration

Create `~/.ptrclaw/config.json`:

```json
{
  "default_provider": "anthropic",
  "default_model": "claude-sonnet-4-20250514",
  "default_temperature": 0.7,
  "anthropic_api_key": "sk-ant-...",
  "openai_api_key": "sk-...",
  "openrouter_api_key": "sk-or-...",
  "ollama_base_url": "http://localhost:11434",
  "compatible_base_url": "http://localhost:8080/v1",
  "agent": {
    "max_tool_iterations": 10,
    "max_history_messages": 50,
    "token_limit": 128000
  },
  "channels": {
    "telegram": {
      "bot_token": "123456:ABC-DEF...",
      "allow_from": ["alice", "bob"],
      "reply_in_private": true
    },
    "whatsapp": {
      "access_token": "EAA...",
      "phone_number_id": "123456789",
      "verify_token": "my-verify-secret",
      "allow_from": ["+1234567890"]
    }
  }
}
```

Environment variables override the config file:

| Variable | Description |
| --- | --- |
| `ANTHROPIC_API_KEY` | Anthropic API key |
| `OPENAI_API_KEY` | OpenAI API key |
| `OPENROUTER_API_KEY` | OpenRouter API key |
| `OLLAMA_BASE_URL` | Ollama server URL (default `http://localhost:11434`) |
| `COMPATIBLE_BASE_URL` | Base URL for OpenAI-compatible endpoint |
| `TELEGRAM_BOT_TOKEN` | Telegram bot token (overrides config) |
| `WHATSAPP_ACCESS_TOKEN` | WhatsApp Business API access token |
| `WHATSAPP_PHONE_ID` | WhatsApp Business phone number ID |
| `WHATSAPP_VERIFY_TOKEN` | WhatsApp webhook verification token |

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

### How to get WhatsApp Cloud API credentials

Use Meta's WhatsApp Business Platform (Cloud API).

1. Go to [Meta for Developers](https://developers.facebook.com/), create/select an app.
2. Add the **WhatsApp** product to the app.
3. In **WhatsApp > API Setup**, copy:
   - **Temporary access token** (for testing) or create a **system user permanent token**
   - **Phone number ID**
4. Configure webhook in **WhatsApp > Configuration**:
   - Callback URL: your public webhook endpoint
   - Verify token: choose a secret string (you define this)
   - Subscribe to `messages` (and other events you need)
5. Add values to env/config:

```sh
export WHATSAPP_ACCESS_TOKEN="EAA..."
export WHATSAPP_PHONE_ID="123456789"
export WHATSAPP_VERIFY_TOKEN="your-verify-secret"
```

Notes:
- Temporary tokens expire; use a long-lived token for production.
- Your webhook URL must be publicly reachable over HTTPS.

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
-m, --message MSG    Send a single message and exit
--channel NAME       Run as a channel bot (telegram, whatsapp)
--provider NAME      Use a specific provider (anthropic, openai, ollama, openrouter)
--model NAME         Use a specific model
-h, --help           Show help
```

## Development

```sh
make build          # compile (all features)
make build-minimal  # slim build: openai + telegram + tools only (~436 KB)
make build-static   # static binary for distribution
make test           # run unit tests (Catch2)
make lint           # run clang-tidy
make coverage       # generate HTML coverage report
make clean          # remove build artifacts
```

### Feature flags

Every provider, channel, and tool is a compile-time feature flag in `meson_options.txt` (all default `true`). Disable unused components to reduce binary size:

| Flag | Controls | Default |
| ---- | -------- | ------- |
| `with_anthropic` | Anthropic provider | `true` |
| `with_openai` | OpenAI provider + SSE parser | `true` |
| `with_openrouter` | OpenRouter provider (implies `with_openai`) | `true` |
| `with_compatible` | OpenAI-compatible provider (implies `with_openai`) | `true` |
| `with_ollama` | Ollama provider | `true` |
| `with_telegram` | Telegram channel | `true` |
| `with_whatsapp` | WhatsApp channel | `true` |
| `with_tools` | All built-in tools | `true` |

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

| Configuration | Size (macOS arm64) |
| ------------- | ------------------ |
| Full (all features) | ~514 KB |
| Minimal (`make build-minimal`) | ~436 KB |

LTO is enabled by default. Static builds (`make build-static`) are slightly larger but fully self-contained.

## Project structure

```text
src/
  main.cpp              CLI entry point and REPL
  agent.hpp/cpp         Agentic loop — chat, tool dispatch, history compaction
  provider.hpp/cpp      Provider interface and types (ChatMessage, ChatResponse, ToolCall)
  tool.hpp/cpp          Tool interface, ToolSpec, ToolResult
  channel.hpp/cpp       Channel interface, ChannelMessage
  config.hpp/cpp        Config loading (~/.ptrclaw/config.json + env vars)
  plugin.hpp/cpp        Self-registration plugin registry (providers, channels, tools)
  event.hpp             Event types for the publish/subscribe bus
  event_bus.hpp/cpp     Thread-safe publish/subscribe event bus
  stream_relay.hpp/cpp  Bridges stream events to progressive channel message editing
  dispatcher.hpp/cpp    XML tool-call parsing for non-native providers
  session.hpp/cpp       Multi-session management with idle eviction
  prompt.hpp/cpp        System prompt builder
  http.hpp              HttpClient interface
  http_socket.cpp       HTTP backend (POSIX sockets + OpenSSL; Linux and macOS)
  util.hpp/cpp          String/path utilities
  channels/
    telegram.hpp/cpp    Telegram Bot API (long-polling, Markdown→HTML, streaming edits)
    whatsapp.hpp/cpp    WhatsApp Business Cloud API (webhooks)
  providers/
    anthropic.cpp       Anthropic Messages API (streaming)
    openai.cpp          OpenAI Chat Completions API (streaming)
    openrouter.cpp      OpenRouter (inherits OpenAI)
    ollama.cpp          Ollama local inference
    compatible.cpp      Generic OpenAI-compatible endpoint
    reliable.cpp        Failover wrapper over multiple providers
    sse.cpp             Server-Sent Events parser
  tools/
    file_read.cpp       Read file contents
    file_write.cpp      Write/create files
    file_edit.cpp       Search-and-replace edits
    shell.cpp           Shell command execution
tests/                  Catch2 unit tests
meson_options.txt       Compile-time feature flags
```

## Acknowledgements

PtrClaw started as a C++ port of [nullclaw](https://github.com/nullclaw/nullclaw) and has since diverged into its own architecture with an event bus, plugin system, and streaming pipeline.
