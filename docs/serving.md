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
`use_oauth` on. Note that the `compatible` and `openrouter` providers do **not** forward
`providers.openai.user` — only the `openai` factory calls `set_user()` — so a gateway that
requires a `user` field must be reached through the `openai` provider.

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

**Effective parallelism is about half the worker count.** `TurnPool` routes an event to
`fnv1a(session_id) % workers`, so a session always lands on the same thread — that is the
whole safety argument for `Agent` and its `ToolManager` having no locks. It also means
random session ids collide while other workers sit idle. Measured, with each provider call
held open 2 s:

| Config | 16 requests | Effective |
| --- | --- | --- |
| `workers: 1` | 32.2 s | 1 |
| `workers: 4`, `max_connections: 8` | 10.1 s | ~3 |
| `workers: 8`, `max_connections: 16` | 6.1 s | ~5 |
| `workers: 16`, `max_connections: 32` (32 requests) | 8.1 s | ~8 |

So size `workers` at roughly **2x the concurrent turns you want**, and `max_connections`
above `workers` again — a connection limit at or below the worker count turns callers who
could be served into callers waiting for a socket.

There is no work stealing, and adding it would cost the lock-free invariant, so uneven
session load leaving workers idle is the design rather than a regression.

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
