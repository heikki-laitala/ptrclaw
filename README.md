# PtrClaw

A minimal C++ port of the [nullclaw](https://github.com/nullclaw/nullclaw) agentic loop.

Nullclaw is a fully autonomous AI assistant infrastructure written in Zig — 678 KB binary, ~1 MB RAM, 22+ providers, 11+ messaging channels, 18+ tools. PtrClaw ports its core agent loop to C++17: provider abstraction, tool dispatch, history compaction, and a terminal REPL.

**362 KB binary (macOS static). 5 providers. 4 tools. 2 messaging channels. C++17.**

## Features

- **Multiple providers** — Anthropic, OpenAI, OpenRouter, Ollama, or any OpenAI-compatible endpoint
- **Native tool use** — providers with function calling use it natively; others fall back to XML-based tool parsing
- **Built-in tools** — `file_read`, `file_write`, `file_edit`, `shell`
- **Interactive REPL** with slash commands (`/status`, `/model`, `/clear`, `/help`, `/quit`)
- **Single-message mode** — pipe a question in and get an answer back
- **Automatic history compaction** when token usage approaches the context limit
- **Provider failover** — `reliable` provider wraps multiple backends with automatic fallback
- **Session management** with idle eviction
- **Telegram channel** — run as a Telegram bot with long-polling, user allowlists, Markdown-to-HTML conversion, and per-user sessions
- **WhatsApp channel** — WhatsApp Business Cloud API integration with webhook payload parsing, E.164 phone normalization, and sender allowlists

## Quick start

> Dev workflow: pull latest `main`, create a feature branch, push, and open a PR.

```sh
git clone <repo-url> && cd ptrclaw
make deps    # install meson, llvm, libcurl
make build   # compile (output: builddir/ptrclaw)

export ANTHROPIC_API_KEY=sk-ant-...
./builddir/ptrclaw
```

## Requirements

- C++17 compiler (Clang 15+ or GCC 10+)
- [Meson](https://mesonbuild.com/) + Ninja
- libcurl

### macOS

```sh
brew install meson llvm
```

### Linux (Debian/Ubuntu)

```sh
sudo apt-get install g++ meson ninja-build libcurl4-openssl-dev clang-tidy
```

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
| `TELEGRAM_BOT_TOKEN` | Telegram bot token (overrides config) |
| `WHATSAPP_ACCESS_TOKEN` | WhatsApp Business API access token |
| `WHATSAPP_PHONE_ID` | WhatsApp Business phone number ID |
| `WHATSAPP_VERIFY_TOKEN` | WhatsApp webhook verification token |

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
make build      # compile
make test       # run unit tests (Catch2)
make lint       # run clang-tidy
make coverage   # generate HTML coverage report
make clean      # remove build artifacts
```

## Project structure

```text
src/
  main.cpp              CLI entry point and REPL
  agent.hpp/cpp         Agentic loop — chat, tool dispatch, history compaction
  provider.hpp/cpp      Provider interface, role_to_string, factory
  tool.hpp/cpp          Tool interface, ToolSpec, built-in tool registry
  config.hpp/cpp        Config loading (~/.ptrclaw/config.json + env vars)
  dispatcher.hpp/cpp    XML tool-call parsing and dispatch
  session.hpp/cpp       Multi-session management
  prompt.hpp/cpp        System prompt builder
  http.hpp/cpp          HttpClient interface (libcurl implementation)
  util.hpp/cpp          String/path utilities
  channel.hpp/cpp       Channel interface, ChannelMessage, ChannelRegistry
  channels/
    telegram.hpp/cpp    Telegram Bot API (long-polling, Markdown→HTML)
    whatsapp.hpp/cpp    WhatsApp Business Cloud API (webhooks)
  providers/
    anthropic.cpp       Anthropic Messages API
    openai.cpp          OpenAI Chat Completions API
    openrouter.cpp      OpenRouter (OpenAI-compatible)
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
```

## Acknowledgements

PtrClaw is a C++ port of [nullclaw](https://github.com/nullclaw/nullclaw), the smallest fully autonomous AI assistant, originally written in Zig.
