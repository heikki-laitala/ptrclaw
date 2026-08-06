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

That is `-Dwith_serving=true -Dwith_tools=false -Dwith_file_read=false -Dwith_http=true`.
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

`nm builddir-serving/ptrclaw | grep ShellTool` returns nothing: the code is absent, not
merely unregistered.

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
- a symlink inside the workspace pointing out is refused, since the link is resolved first
- `/work/sessions/abc-evil` is refused for a session rooted at `/work/sessions/abc`, which a
  prefix comparison would have accepted

Relative paths resolve against the session's own workspace, so `file_write("notes.md")`
cannot reach the shared directory by accident. A write whose path resolves into
`context_dir` is **refused, not redirected**: the model sees the failure instead of silently
writing elsewhere.

### Staging context files

The context manager writes into `context_dir`; sessions only read it. Parallel reads need no
coordination — separate opens of one file are safe, so a session's read is unaffected by how
many others are reading it.

One requirement: when **updating** a file while turns are running, write to a temporary name
and `rename()` into place. A plain truncating write can be observed half-finished by a
session reading mid-write. This is the same atomicity ptrclaw uses for its own stores
(`atomic_write_file` in `src/util.cpp`).

## Pod configuration

```json
{
  "workers": 8,
  "memory": { "isolation": "session" },
  "channels": { "http": { "listen": "0.0.0.0:8080", "max_connections": 16 } },
  "agent": { "session_max_idle_seconds": 900 },
  "allow_channel_commands": false,
  "serving": {
    "context_dir": "/work/context",
    "workspace_root": "/work/sessions",
    "generate_session_ids": true
  }
}
```

Two of these are easy to miss and change everything:

- **`workers` defaults to 1**, which means turns run inline, one at a time for the whole
  process — simultaneous chats serialise. Raise it to run them in parallel. Turns are
  sharded by session id, so a session stays serialised with itself; it is not work-stealing,
  so uneven session load leaves workers idle.
- **`memory.isolation` defaults to `"shared"`**, which means every session reads and writes
  one memory store and recalls the others' content. Set `"session"` for isolated tasks.

`max_connections` governs how many callers can be *waiting*, so keep it above `workers`.
`allow_channel_commands` stays false: slash commands are the operator's surface, and `/auth`
is refused on channels regardless.

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

The channel still refuses a second concurrent turn for one session with 409: two turns
interleaving over one history is not a conversation.

## Known limits

- **Workspaces are never deleted.** Eviction drops the session's in-memory state, not its
  directory — matching the memory stores, which outlive a session on purpose. For ephemeral
  tasks that is unbounded disk growth in the pod, and there is no retention mechanism yet.
- **Eviction briefly stops polling.** Sessions are freed only when no worker is mid-dispatch,
  so the poll loop drains the turn pool first. Under steady load it waits for the deadline
  and then blocks until every queue is empty — a periodic latency spike.
- **Sessions live in the process.** Scaling past one pod needs session affinity at the
  ingress; there is no shared session state.
- **`file_edit` has no scoped counterpart**, so a serving build cannot do targeted edits —
  only whole-file writes.
