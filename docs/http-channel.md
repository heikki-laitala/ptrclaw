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
- **`max_connections`** — how many callers may be *connected* at once. Parallel turns are
  controlled separately by the top-level `workers` key; see "Concurrency" below.
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

## Concurrency

`max_connections` governs concurrent *connections*. Concurrent *turns* are governed by
the top-level `workers` key, which defaults to `1`:

```json
{
  "workers": 4
}
```

At `workers: 1` PtrClaw publishes each inbound message on the event bus synchronously
from the poll loop, so `Agent::process` — provider call, tool loop and all — runs to
completion before the next message is looked at. One turn executes per process.

Above `1`, turns are handed to a pool of worker threads sharded by session id. Turns for
**different** sessions run in parallel; turns for **one** session always land on the same
worker, so they stay serialised and in order. That sharding is the safety argument, not
an optimisation: `Agent` has no internal synchronisation, and the shard guarantees no two
threads ever touch one `Agent`.

The per-session `409` above is the other half of the same rule — a second concurrent turn
on one session is refused rather than queued, because two turns interleaving over one
history is not a conversation.

Practical guidance:

- Throughput per process is roughly `workers / turn duration`, and turns are dominated by
  waiting on the provider, so workers cost little CPU. Start at the number of concurrent
  callers you expect, not the number of cores.
- Raising `max_connections` converts *refused* connections into *waiting* ones — worth
  doing, since the kernel accept queue is only 16 deep and the 17th caller is refused
  outright — but on its own it does not raise throughput.
- To serve more traffic still, run more processes. Nothing is held authoritatively in one
  when the caller pushes `history`, so any process can serve any turn.
- Session ids come straight from the request body. With `memory.isolation: "session"`
  each distinct id gets its own store on disk, so an untrusted caller can create
  directories — put authentication in front of it (`secret`, or the reverse proxy).

## Behind a reverse proxy

Same guidance as [reverse-proxy.md](./reverse-proxy.md), with one addition: do not let
the proxy buffer or rewrite `text/event-stream` responses. The `X-Accel-Buffering`
header covers nginx; other proxies may need `proxy_buffering off` or an equivalent.

## Slash commands are off on this channel

The HTTP channel carries traffic from whoever can reach it, so PtrClaw's slash commands
are not dispatched for it unless `allow_channel_commands` is set to `true` in the config
(see the README). A message beginning with `/` is passed to the agent as ordinary text.

The `session` field in the request body is a **routing key, not an identity**: the caller
picks it, so it confers nothing. In particular, naming a session `cli` does not make a
request local — commands are enabled by trusted, channel-side metadata, never by the
session string.

Leave it off for anything public. The commands include `/model` and `/provider`, which
change what the agent runs on, `/memory import`, which writes memory entries — including
the `soul:identity` entry that gives a provisioned agent its identity — and `/auth`, which
sets a provider API key.
