# Serving profile: many sessions in one process

One pod, one ptrclaw, many simultaneous chats or tasks over the HTTP channel. Each caller
supplies a session id and gets its own conversation; every session reads one shared
directory of context staged from outside, and writes only into its own workspace.

This is a build profile, not a runtime switch. The personal-agent build is unaffected and
carries none of it.

## Build

```sh
make build-serving      # binary at builddir-serving/ptrclaw
make test-serving       # the profile's own assertions
```

That is `-Dwith_serving=true -Dwith_tools=false -Dwith_file_read=false -Dwith_http=true`,
plus `-Dwith_anthropic=false -Dwith_ollama=false -Dwith_openrouter=false
-Dwith_compatible=false` — **the pod talks to OpenAI and nothing else**, over chat
completions and the ChatGPT-subscription OAuth flow.

To re-enable one, override the variable rather than reconfiguring the build directory by
hand — the target re-applies these flags on every run, so a `meson configure` would be
undone by the next `make build-serving`:

```sh
make build-serving SERVING_PROVIDERS="-Dwith_anthropic=true"
```

An OpenAI-compatible *gateway* needs no extra provider: point `providers.openai.base_url`
at it and the `openai` provider posts to `<base_url>/chat/completions`. A custom base URL
also wins over the Responses API, so a gateway keeps receiving chat completions even with
`use_oauth` on.

A gateway that meters per caller usually requires a `user` field — hirebell-llm answers
`400 missing_user` without one. Every OpenAI-dialect provider forwards it, and each reads
**its own** config entry, so put the identifier under the provider you actually use:

| Provider in use | Field |
| --- | --- |
| `openai` | `providers.openai.user`, or the `OPENAI_USER` environment variable |
| `compatible` | `providers.compatible.user` |
| `openrouter` | `providers.openrouter.user` |

Unset, the key is omitted from the request rather than sent empty, so an endpoint that
validates it sees a missing field.

`Config`'s default provider follows the build (`src/config.hpp`), so a pod config that omits
`"provider"` still starts on OpenAI rather than failing with `Unknown provider: anthropic`.
CI builds and tests this exact configuration — a trimmed binary is not merely a smaller one,
and the suite has to hold where Anthropic cannot be constructed.
The tool flags are not decoration: `with_serving` registers `file_read` and `file_write`
scoped to the calling session, and the unscoped tools register the same names —
`PluginRegistry` keys factories by name, so having both would leave static-init order to
decide which one a session got. meson refuses the combination rather than resolving it.

What the profile leaves out, and why:

| Tool | Reason |
| --- | --- |
| `shell` | Forks `/bin/sh -c` with no filtering and the full environment, so any session could read `~/.ptrclaw/config.json` — the pod's provider keys and OAuth refresh token. A workspace root cannot fence a shell. |
| `cron` | Replaces the entire crontab on every write, so concurrent sessions clobber each other; `list` returns other tenants' entries; the scheduled command is never validated and outlives the process. |
| `file_edit` | No scoped counterpart yet. Read and write cover the cases the profile is for. |

The code is absent rather than merely unregistered, and `tests/test_serving_profile.cpp`
asserts it by inspecting the registry in a build of this profile. Note that `nm` cannot
show you this: `make build-serving` strips the binary, so every symbol name is gone and
`nm | grep ShellTool` is empty whether or not the tool was compiled in. The behavioural
check is to ask the pod — a serving binary answers `CANNOT-EXECUTE` when told to run a
command, and lists exactly `file_read` and `file_write` when asked what tools it has.

## Binary and memory footprint

Measured on macOS arm64, stripped:

| | pod binary | idle RSS | 200 sessions |
| --- | --- | --- | --- |
| before (`-O3`, every provider) | 948 KB | ~5.8 MB | ~10.7 MB |
| size flags only (`-Os`, `NDEBUG`) | 565 KB | ~5.6 MB | ~10.2 MB |
| as shipped (also OpenAI-only) | **548 KB** | ~5.6 MB | ~10.2 MB |

The binary is the part that moves: `-O3` inlines for speed and costs ~40% of the image
here. Resident memory barely does, and it is worth being precise about why. Idle RSS drops
by ~0.2 MB — smaller text, fewer resident pages — and that difference held across every
repeat. Per-session cost did **not** change: ~19 KB either way, across three runs each,
with the run-to-run spread (16–22 KB) wider than any difference between the two builds.
Session state is heap, and an optimization level does not touch it.

So a pod holding 200 live conversations sits around 10 MB, of which ~5.6 MB is fixed
(libcurl, TLS, the worker stacks) and ~4 MB is the sessions themselves. `agent.max_sessions`
is the knob that bounds it.

### Trimming further

`-Os` and the OpenAI-only provider set are applied for you. What is left costs capability
rather than dead weight, so the profile stops here — the prices, measured against the
548 KB build:

| Also disabled | Binary | Gives up |
| --- | --- | --- |
| `-Dwith_openai_oauth=false` | 532 KB | ChatGPT-subscription auth; API keys only |
| `-Dwith_sqlite_memory=false` | 514 KB | the FTS5 backend, if you turn memory on |
| both | 514 KB | |

Roughly 6% for a pod that cannot use a subscription and cannot ever turn memory on. The
OAuth flow is what this deployment authenticates with, so it stays.

The two do not add up, and the table says what was measured rather than the sum: dropping
SQLite alone already reaches 514 KB. Under LTO the OAuth code and the SQLite backend pull
in overlapping machinery, so removing either one collects most of the same dead weight.

## The two roots

| Root | Scope | A session may |
| --- | --- | --- |
| `context_dir` | one directory, shared by every session | **read** |
| `workspace_root/<key>` | that session's own | **read and write** |

```json
{
  "serving": {
    "context_dir": "/work/context",
    "workspace_root": "/work/sessions",
    "generate_session_ids": true
  }
}
```

`<key>` is `session_store_key()` — a 16-hex FNV-1a of the raw session id followed by the id
sanitised to `[A-Za-z0-9._-]` and truncated. The same component the per-session memory
stores use, so one session's workspace and memory sit under matching names. Session ids come
straight from the request body, which makes this a path-traversal boundary: the hash prefix
means the component can never *be* `.` or `..`, and the substitution means it can never
contain a separator.

Both keys are empty by default. With no `workspace_root` there is no writable directory;
with neither root the scoped tools refuse every path rather than falling back to the process
working directory.

### What "scoped" means

Paths are checked after resolution, not by spelling. `resolve_in_workspace()`
(`src/workspace.cpp`) runs `std::filesystem::weakly_canonical` and then requires the result
to be inside a permitted root, compared with `lexically_relative` rather than a string
prefix. So:

- an absolute path outside both roots is refused — including `/etc/passwd`
- `../../elsewhere/x` is refused; `sub/../notes.md` is allowed, because it stays inside
- a symlink inside the workspace pointing out is refused, **including one whose target does
  not exist yet** — `weakly_canonical` alone leaves those unresolved, so every component is
  expanded with `lstat` semantics, a substituted target's own components are re-examined in
  turn (a link to `dirlink/missing` cannot slip past by hiding the link in its target), and a
  chain is bounded against loops
- `/work/sessions/abc-evil` is refused for a session rooted at `/work/sessions/abc`, which a
  prefix comparison would have accepted
- if the roots overlap — `context_dir=/work` with `workspace_root=/work/sessions` — a read
  that lands in *another* session's workspace is refused even though it is inside
  `context_dir`. The shared read reaches only what is genuinely shared.

Relative paths resolve against the session's own workspace, so `file_write("notes.md")`
cannot reach the shared directory by accident. A write whose path resolves into
`context_dir` is **refused, not redirected**, and the refusal says the shared context is
read-only rather than claiming the path is outside the roots — the prompt lists that
directory, so a misleading message would leave the model with no way to recover.

Reading a directory is an error, not empty success, and a read stops at the 50 KiB cap
instead of loading the whole file first — one oversized file in the shared context would
otherwise be able to exhaust a pod every session shares.

### Staging context files

The context manager writes into `context_dir`; sessions only read it. Parallel reads need no
coordination — separate opens of one file are safe, so a session's read is unaffected by how
many others are reading it.

One requirement: when **updating** a file while turns are running, write to a temporary name
and `rename()` into place. A plain truncating write can be observed half-finished by a
session reading mid-write. This is the same atomicity ptrclaw uses for its own stores
(`atomic_write_file` in `src/util.cpp`).

## Pod configuration

A serving build already defaults to the values below — this is what you get with none of
them set, so the block is here to be read and overridden, not copied:

```json
{
  "workers": 8,
  "channels": { "http": { "listen": "0.0.0.0:8080", "max_connections": 32 } },
  "agent": { "session_max_idle_seconds": 900, "max_sessions": 200 },
  "allow_channel_commands": false,
  "serving": {
    "context_dir": "/work/context",
    "workspace_root": "/work/sessions",
    "generate_session_ids": true
  }
}
```

The three capacity defaults differ from the personal agent, which serves one person and is
right to serialise:

- **`workers`: 8** instead of 1. At 1 the pool runs turns inline, one at a time for the
  whole process, so simultaneous chats serialise completely.
- **`max_connections`: 32** instead of 8, above `2 x workers` — see the sizing section for
  why the multiplier is 2.
- **`max_sessions`: 200** instead of 0 (unlimited), and **`session_max_idle_seconds`: 900**
  instead of an hour. With generated ids the session count is chosen by whoever is calling,
  so unlimited means memory growth an operator cannot bound.

Every one is a default, not a policy: an explicit value in config always wins.

- **`memory.backend`** defaults to `"none"` in a serving build. A pod's per-session store is
  write-only in practice: the session records facts, ends, and nobody returns to that id — so
  it would pay an embedding call per turn, a synthesis call every few turns, and three files
  on disk for something never read again. `"none"` also removes the `memory_*` tools from the
  tool list, so the model is not offered a store it has no use for.
- **`memory.isolation`** defaults to `"session"` in a serving build (and `"shared"`
  everywhere else), so a pod that *does* enable a backend is isolated without extra
  configuration.

`allow_channel_commands` stays false: slash commands are the operator's surface, and `/auth`
is refused on channels regardless.

### Concurrency: three different limits

They are routinely confused, and only one of them is about sessions.

| Limit | Set by | What it bounds |
| --- | --- | --- |
| Sessions **held** | `agent.max_sessions` | conversations in memory at once — a memory question |
| Turns **running** | `workers` | how many turns execute in parallel |
| Callers **connected** | `channels.http.max_connections` | open connections; past it the acceptor stops accepting and the kernel backlog queues |

**Sustained throughput is `workers / turn_latency`.** Measured at the defaults against a
provider held open 1.5 s per call, clients looping on their own session:

| Clients | Throughput | p50 | p95 |
| --- | --- | --- | --- |
| 1 | 0.66 turns/s | 1.51 s | 1.51 s |
| 4 | 2.66 turns/s | 1.51 s | 1.51 s |
| 8 | **5.30 turns/s** | 1.51 s | 1.51 s |
| 16 | 4.98 turns/s | 3.01 s | 4.52 s |
| 32 | 4.75 turns/s | 4.52 s | 9.04 s |
| 64 | 5.06 turns/s | 7.53 s | 12.05 s |

Service capacity flattens at ~5 turns/s — `8 workers / 1.5 s` — and past 8 clients the extra
load turns into latency, not throughput: p50 doubles at 16 and quintuples at 64 while the
completion rate holds. That is the queue being honest, and it is the shape to watch for in
production: rising p95 at a flat request rate means `workers` is the binding constraint.

So the throughput a pod can serve is roughly:

```
turns/s ≈ workers / seconds_per_turn
```

At the default 8 workers: ~5/s for a 1.5 s turn, ~2.7/s at 3 s, ~0.8/s at 10 s. Raise
`workers` for more — it costs almost nothing in memory — and keep `max_connections` above
it, since a connection limit at or below the worker count turns callers who could be served
into callers waiting for a socket.

### How many requests can be in flight

Three limits stack, and only the first is about throughput. Measured at the defaults with a
provider held open 8 s:

| Fired at once | Served | Failed |
| --- | --- | --- |
| 32 | 32 | 0 |
| 48 | 48 | 0 |
| 64 | 48 | **16** |

- **8 execute** — `workers`.
- **32 are connected** — `max_connections`. The rest hold an open connection and wait.
- **+`max_connections` more wait in the kernel accept queue**, so 48 are in the system at
  the defaults. Past that the handshake is dropped and the caller sees a reset rather than a
  status code — the acceptor deliberately does not answer 503, because that would turn a
  wait into an error for everyone over the line.

Raising `max_connections` raises both halves; the accept queue tracks it (`listen_backlog`).

A pod configured wide — `workers: 512`, `max_connections: 2000`, `max_sessions: 5000` —
accepts **1000 concurrent requests with zero failures at ~43 MB**. How fast it drains them
is set by `workers`, and by how evenly the session ids hash across them:

| Concurrent | Wall (3 s per turn) | Effective concurrency |
| --- | --- | --- |
| 25 | 3.0 s | 25 |
| 50 | 3.0 s | 50 |
| 100 | 3.0 s | 99 |
| 250 | 3.0 s | 246 |
| 500 | 3.1 s | 485 |
| 1000 (`workers: 1024`) | 3.1 s | 968 |

Perfect parallelism is 3 s, and the pod is within a rounding error of it at every level: a
burst of N turns takes about as long as one turn, up to the ceiling `workers` sets.

**Threads exist only while turns do.** A pod configured for 1024 concurrent turns holds four
threads and ~5 MB while nothing is happening — idle cost does not track the ceiling, so
there is no reason to keep it tight. What costs is turns actually in flight: ~57 KB for a
one-line prompt, ~300 KB for an 8 KB one. A pod running 1000 at once peaked at **61 MB**.

The defaults are not this pod. The same 1000-request burst against `workers: 8` and
`max_connections: 32` loses 928 of them: 48 fit in the system and the rest are refused at
the socket. Concurrency of this order is a configuration, not something to expect for free:

```json
{ "workers": 1024,
  "channels": { "http": { "max_connections": 1200 } },
  "agent": { "max_sessions": 2000 } }
```

**One session is still one turn at a time.** `TurnPool` runs a session's turns in arrival
order and never concurrently — the whole safety argument for `Agent` and its `ToolManager`
having no locks. A second request for a session already running is queued behind the first,
not run beside it, which is the same thing the channel's 409 tells a caller directly.

Concurrency is therefore across *sessions*. A single session cannot be made faster by adding
workers, and a caller that funnels unrelated work through one session id gets it serialised.

### Sizing against a memory limit

Measured on the shipped binary against a stub provider, so these are the pod's own costs
rather than the model's:

| Component | Cost |
| --- | --- |
| Fixed baseline | **~5.5 MB** |
| Each worker | **~0** — 1 to 64 workers moved resident memory under 1 MB |
| Session, one short turn | **~16 KB** |
| Session carrying history | **~1.9 KB per turn**, bounded by `agent.max_history_messages` (50) |
| Turn in flight | **~250-420 KB** with an 8 KB prompt |

Which gives:

```
RSS ≈ 5.5 MB + (sessions × 16 KB … 100 KB) + (turns_in_flight × 300 KB)
```

The defaults land at **~5.5 MB idle** and a ceiling near **30 MB** — 200 sessions each
carrying a full history window, plus 8 turns in flight. Measured points on the curve, one
short turn per session: 100 sessions 9.4 MB, 500 sessions 14.6 MB, 1000 sessions 21.6 MB.

Two things follow that are worth knowing before tuning:

- **Concurrency is cheap; sessions held are not.** Workers cost nothing at rest, so raising
  `workers` for throughput is close to free. It is `max_sessions` and the idle window that
  decide the pod's memory ceiling.
- **The idle window is the cheaper lever.** A session freed early costs the caller a
  reconstruction, not a conversation — it can push history back with the next request. So
  shortening `session_max_idle_seconds` reclaims memory without losing anything, while
  lowering `max_sessions` makes the pod refuse work.

For a pod with a 128 MB limit the defaults leave roughly 4x headroom. If the limit is
tighter, cut the idle window first; if traffic is bursty and the limit is generous, raise
`max_sessions` and leave the rest alone.

### Turning memory back on

Both memory keys are defaults, not locks. A pod serving conversations a caller returns to
says so, and gets a store isolated per session:

```json
{ "memory": { "backend": "sqlite", "isolation": "session" } }
```

Two things come with that, and neither is bounded today: `sessions/<key>/` is never deleted
(eviction drops the instance, not the files), and enrichment runs on every turn — with
`embeddings.provider` set that is an embedding call per turn, against a store that starts
empty for every new session.

`agent.max_sessions` (0 = unlimited) bounds how many sessions exist at once. It matters most
with generated ids: a caller that never echoes the announced id back mints a session per
request, each holding an Agent, a Config copy and a memory backend until idle eviction. A
session beyond the cap is **refused**, not evicted — freeing one could pull an Agent out from
under a worker mid-turn — so the caller gets an error and the pod stays up.

`memory.isolation` needs no setting in a serving build: it defaults to `"session"` there —
in the generated config file as well as the struct, since `Config::load()` merges the
defaults into the file and then parses the result — so the fenced filesystem is not paired
with a memory store every tenant shares. An explicit `"shared"` still wins, for a pod running
one tenant's own tasks.

## No identity interview

A fresh install normally answers its first channel message with the soul-hatching interview —
a few questions about what to call the assistant. That is right for a personal agent, where
the first visitor is the operator, and it is decided by whether the memory store has a soul.

Under `memory.isolation: "session"` that question is asked again for *every* session, so a
pod would open each one with the ceremony instead of the work, and the interview replaces the
entire system prompt, tools included — the session cannot do anything until somebody answers.
Nobody at the far end of an HTTP request will.

So auto-hatching is skipped whenever a workspace or shared context is configured — a
deployment stating it serves callers rather than a person. `/hatch` still works on the CLI,
where an operator is present to answer.

Per-session memory is deliberately *not* the signal: session isolation is a storage layout,
and `docs/memory.md` documents one interview per chat under it. A Telegram user is a person
who can answer one, and `/hatch` sits behind `allow_channel_commands`, so keying on isolation
would leave those chats no way to create an identity at all.

### Giving the pod an identity instead

A served session that never hatches has no soul, so it answers as the default assistant.
Configure the identity rather than interviewing for it:

```json
{ "agent": { "persona": {
    "identity": "You are Atlas, a terse task runner for a document pipeline.",
    "user": "An automated context manager, not a person. It cannot answer questions.",
    "philosophy": "Do the task, answer in one line."
} } }
```

The three parts are the ones hatching writes into memory, and they render the same
`## Your Identity` block — so a configured pod and a hatched personal agent describe
themselves identically. A configured persona also suppresses hatching on its own, shared
store or not, and at every automatic entry point — the channel message, `/start`, and the
REPL's first-run prompt. `is_hatched()` only reads memory, so each of those would otherwise
look at a configured pod and see an agent with no identity. Explicit `/hatch` still runs the
interview: an operator who asks for it by name gets it.

Note the alternative and why it is not this. A caller *can* push a system message as
`history[0]`, but `Agent::set_history` then treats it as the whole prompt and ptrclaw's own
is never injected — so the session loses the Workspace section naming its roots, and the
model starts guessing at paths its tools will refuse. A configured persona is added to the
built-in prompt instead of replacing it.

## Session ids

```
POST /chat  {"session": "task-42", "message": "...", "history": [...]}
```

`session` is required as always. With `serving.generate_session_ids` on, a request may omit
it — absent, `null`, or `""` — and the pod invents one, announced as the first frame of the
stream:

```
event: session
data: {"session":"a1b2c3d4e5f60718"}

event: token
data: {"delta":"Hel"}
```

In the stream rather than a response header because a browser `EventSource` client cannot
read headers. A caller that supplied its own id sees no `session` frame — the stream is
byte-for-byte what it was before.

Generated ids come from `secure_random_hex()`, not the `mt19937` used for tool-batch ids:
under this profile the id selects a private workspace and memory store, so it is a capability
and must not be predictable from other ids the pod has handed out.

The channel still refuses a second concurrent turn for one session with 409: two turns
interleaving over one history is not a conversation.

## History: who owns it

A session keeps its conversation in memory across turns, and a request may also carry a
`history` window. Both work, and the choice decides how much the pod is allowed to forget.

**The pod remembers.** Send a message with no `history`. The session accumulates the
conversation, bounded by `agent.max_history_messages` and compacted when it approaches the
token limit.

```
POST /chat  {"session": "task-42", "message": "and what about the second one?"}
```

**The caller remembers.** Send `history` with every request. `Agent::set_history()`
**replaces** whatever the session had accumulated, and the new message is appended to it.

```
POST /chat  {"session": "task-42", "message": "...", "history": [ ... ]}
```

This is the shape to use when something outside the pod already owns the transcript — a
context service in front of several agents, a queue that replays work, anything that can
reconstruct the conversation. It makes the pod stateless per turn: eviction costs nothing, a
restart costs nothing, and the session id narrows to what it uniquely provides — a private
workspace and one turn at a time.

**What to avoid is the hybrid**: pushing context on the first message and relying on the
session afterwards. It works until `agent.session_max_idle_seconds` passes — 15 minutes by
default — and then the session is evicted and the next message arrives at an agent with no
memory of the conversation. No error, no warning, just amnesia, and only for the users who
paused. A pod restart does the same thing at any moment, since `memory.backend` defaults to
`"none"` and nothing is written down.

So: push history every turn, or never. If you push it every turn, the eviction settings stop
being a correctness question and become purely a memory one.

### Pushed history and tool calls

A turn that ran tools is not just text. Where the transcript is owned outside the pod — a
caller that pushes `history` with every request rather than letting the session accumulate it
— both halves of the exchange have to cross the boundary, or a replayed conversation
reconstructs as though no tool had run and the model loses what the tool told it.

**Out**, on the same stream as the tokens and in order with them:

```
event: tool_call
data: {"id":"call_1","name":"file_read","arguments":"{\"path\":\"notes.md\"}"}

event: tool_result
data: {"id":"call_1","name":"file_read","success":true,"output":"Deadline: 30 April."}
```

`arguments` is the raw JSON string the model produced rather than a re-encoded object, so
what a caller stores is exactly what was sent. New event types are additive for SSE — a
client subscribes to the names it knows — so one reading only `token` and `done` is
unaffected by these.

**Back in**, as two entry shapes `history` accepts:

```json
{"role": "assistant", "content": "",
 "tool_calls": [{"id": "call_1", "name": "file_read", "arguments": "{\"path\":\"notes.md\"}"}]}
{"role": "tool", "content": "Deadline: 30 April.", "tool_call_id": "call_1", "name": "file_read"}
```

They map onto the representation the agent builds for its own history, so the providers
replay them the way they replay a turn they ran themselves — Anthropic as `tool_use` and
`tool_result` blocks, OpenAI as a `tool_calls` array with `role: "tool"` answers.

**The pairing is checked, and an unbalanced window is a 400 naming the id.** A result whose
call is absent, or a call with no result, is rejected by the provider outright, so it is
caught here where the error can say which id is at fault:

```
{"error":"tool result 'call_1' answers no preceding tool call"}
{"error":"tool call 'call_1' has no result in this window"}
```

Two things worth deciding deliberately:

- **Size.** Tool output can be large, and replaying it costs the wire on every turn.
  `agent.max_history_messages` bounds what the pod keeps; nothing bounds what a caller
  chooses to send. Truncating an old tool result is reasonable — but truncate its *content*
  and keep the message, or the window becomes unbalanced and is refused.
- **What it is for.** A tool result is the only record of what the agent saw. Dropped from
  the transcript, a later turn cannot tell what the file said — only what the assistant
  claimed about it.

The **workspace** is the other durable channel, and often the better one for bulk: a
session's directory derives from its id, so it survives eviction and restart, and a tool can
write there instead of returning everything through the conversation.

One trap when pushing: **do not send a system message as `history[0]`**. `set_history()`
then treats it as the whole prompt and ptrclaw's own is never injected — the session loses
the Workspace section naming its roots, and the model starts guessing at paths its tools
will refuse. Configure `agent.persona` instead, which is added to the built-in prompt rather
than replacing it.

## Ending a session

```
POST /session/end  {"session": "task-42"}
→ 202 {"session":"task-42","status":"ending"}
```

The pod cannot tell a finished task from an idle one; only the caller knows. Without this the
sole exit was `agent.session_max_idle_seconds`, which drops the conversation and keeps the
workspace — so a pod running short tasks accumulated a directory per task, permanently.

Ending a session frees its conversation, its memory and cache instances, and **deletes its
workspace directory and everything in it**. Collect whatever the task produced before calling
this. The shared context directory is untouched — no session owns it — and so are the other
sessions' workspaces.

`202`, not `200`: an Agent's event handlers are copied out of the bus before being called, so
freeing one while a worker is mid-turn is a use-after-free. The request marks the session and
the poll loop frees it in the same quiescent window eviction uses — the next iteration once
the pool is idle, or the eviction deadline under sustained load. In between, the id is
refused with an error naming the reason rather than serving either the old conversation or a
new one under a live sibling's tool subscriptions.

The same call is the way to clean up after a task whose session has *already* been evicted
for idleness: the pod has forgotten the conversation but still holds the directory, and an id
it has never heard of is accepted rather than 404'd. `DELETE`-shaped semantics — asking twice
is not an error, and the answer is the same either way.

Use the shared secret if the pod is reachable by more than one tenant. Deleting another
tenant's work product is exactly what it protects.

## Known limits

- **Idle eviction still leaves the workspace behind.** Only `POST /session/end` deletes it,
  so a caller that never says a task is over — or a pod that is killed before the reap runs —
  still accumulates directories. There is no age-based sweep and nothing removes orphans at
  startup; `session_max_idle_seconds` bounds memory, not disk.
- **Memory stores outlive the session on purpose**, and ending one does not delete its store.
  That is the documented behaviour for every backend (`docs/memory.md`), and the serving
  default of `memory.backend: "none"` means there is usually nothing there to remove.
- **Eviction briefly stops polling.** Sessions are freed only when no worker is mid-dispatch,
  so the poll loop drains the turn pool first. Under steady load it waits for the deadline
  and then blocks until every queue is empty — a periodic latency spike.
- **Sessions live in the process.** Scaling past one pod needs session affinity at the
  ingress; there is no shared session state.
- **`file_edit` has no scoped counterpart**, so a serving build cannot do targeted edits —
  only whole-file writes.
