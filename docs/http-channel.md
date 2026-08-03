# The `http` channel — chat over HTTP with SSE

Build with `-Dwith_http=true` (off by default), run with `--channel http`.

For a front end that owns the conversation: it POSTs a message, optionally pushes the
history window with it, and reads the reply as a token stream. Unlike Telegram or
WhatsApp there is no third-party service — the caller is whatever you put in front of it.

## Configuration

`~/.ptrclaw/config.json`:

```json
{
  "channels": {
    "http": {
      "listen": "127.0.0.1:8080",
      "secret": "",
      "max_body": 65536,
      "max_connections": 8,
      "turn_timeout_seconds": 120
    }
  }
}
```

- **`secret`** — when set, every `/chat` request must carry
  `Authorization: Bearer <secret>`. Empty means no authentication, which is only safe on
  a loopback or otherwise private address. `/healthz` is always open, because container
  probes cannot carry it.
- **`max_connections`** — how many callers may be *connected* at once. It does not buy
  parallel turns; see "One turn at a time" below.
- **`turn_timeout_seconds`** — how long a single turn may take before the stream is
  closed with an `error` event. Without it a provider that never answers holds a
  connection open indefinitely.

## Protocol

### `POST /chat`

```json
{
  "session": "abc123",
  "message": "what time is breakfast?",
  "history": [
    {"role": "system",    "content": "You are a hotel concierge."},
    {"role": "user",      "content": "hello"},
    {"role": "assistant", "content": "Good evening!"}
  ]
}
```

- **`session`** (required) — the conversation key. Sessions are independent and are
  evicted after an hour idle.
- **`message`** (required) — the user's turn.
- **`history`** (optional) — when present it **replaces** the session's own history for
  this turn, so the caller can be the single source of truth and the agent stays a
  stateless consumer. A leading `system` message becomes the system prompt. Roles are
  `system`, `user` and `assistant`.

  **`tool` is refused with a `400`**, deliberately. A tool result only means anything
  paired with the `tool_call_id` of the call it answers, and the assistant turn carrying
  the original `tool_calls` is not expressible here either — so accepting the role would
  send unassociated tool results to the provider. Refusing is more honest than
  half-supporting it.

Omit `history` entirely to let the agent accumulate its own, as other channels do. A
malformed `history` is a `400` rather than a request answered without it — being quietly
answered with the context missing is worse than being told.

**One turn per session at a time.** A second `/chat` for a session whose turn is still
running is refused with `409`. Everything downstream keys off the session — the reply and
the token stream both — so two overlapping turns would share one mailbox: both streams
racing for the first reply, one closing empty, the second reply discarded. Wait for
`done` (or `error`) before sending the next message for that session. An abandoned turn is
reclaimed after `turn_timeout_seconds`, so a client that vanishes mid-request cannot wedge
its session permanently.

### The response stream

`Content-Type: text/event-stream`, delimited by connection close.

```
event: token
data: {"delta":"Break"}

event: token
data: {"delta":"fast is at 7."}

event: done
data: {"content":"Breakfast is at 7."}
```

`done` carries the complete reply, so a client that ignores `token` events entirely
still works. Failures arrive as `event: error` with `{"message": "..."}` — including
timeouts.

`X-Accel-Buffering: no` is sent so nginx does not buffer the body; `Cache-Control:
no-cache` alone does not prevent that.

### `GET /healthz`

Returns `200 ok`, no authentication. Suitable for a readiness probe.

## One turn at a time

`max_connections` governs concurrent *connections*, not concurrent turns. PtrClaw
publishes each inbound message on the event bus synchronously from a single poll loop, so
`Agent::process` — provider call, tool loop and all — runs to completion before the next
message is looked at. **One turn executes per process.**

So several callers can be connected and streaming while their turns queue behind one
another. That is deliberate rather than incidental: `Agent` has no internal
synchronisation, and pushing history from a connection thread would race an in-flight
turn, so every `Agent` mutation stays on the poll thread.

Two practical consequences:

- Throughput per process is `1 / turn duration`. Raising `max_connections` converts
  *refused* connections into *waiting* ones — worth doing, since the kernel accept queue
  is only 16 deep and the 17th caller is refused outright — but it does not raise
  throughput.
- To serve more traffic, run more processes. Nothing is held authoritatively in one when
  the caller pushes `history`, so any process can serve any turn.

## Behind a reverse proxy

Same guidance as [reverse-proxy.md](./reverse-proxy.md), with one addition: do not let
the proxy buffer or rewrite `text/event-stream` responses. The `X-Accel-Buffering`
header covers nginx; other proxies may need `proxy_buffering off` or an equivalent.

## Slash commands are off on this channel

The HTTP channel carries traffic from whoever can reach it, so PtrClaw's slash commands
are not dispatched for it unless `allow_channel_commands` is set to `true` in the config
(see the README). A message beginning with `/` is passed to the agent as ordinary text.

Leave it off for anything public. The commands include `/model` and `/provider`, which
change what the agent runs on, `/memory import`, which writes memory entries — including
the `soul:identity` entry that gives a provisioned agent its identity — and `/auth`, which
sets a provider API key.
