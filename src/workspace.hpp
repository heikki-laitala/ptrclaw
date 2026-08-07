#pragma once
#include <optional>
#include <string>

namespace ptrclaw {

// The filesystem a single session may touch.
//
// A pod serving many sessions at once gives each one its own directory to work in, and
// points all of them at one directory of context staged from outside. Both are empty for
// the personal agent, where tools are unscoped and reach whatever the process can.
struct SessionWorkspace {
    // This session's own directory: read and write. Empty means nothing is writable.
    std::string workspace;
    // Shared, and read-only for every session — which is what stops two concurrent tasks
    // clobbering the context they are both working from. Empty means no shared context.
    std::string context_dir;
    // The parent every session's workspace sits under. Needed because the two roots may
    // overlap: with context_dir="/work" and workspace_root="/work/sessions" — a natural
    // layout — the shared read would otherwise reach a sibling session's directory, and
    // the key in its name is an offline-computable hash rather than a secret.
    std::string workspace_root;
};

enum class WorkspaceAccess { Read, Write };

// Resolves `path` against a session's roots, returning the absolute path to use, or
// nullopt when it lies outside what `access` permits: writes only inside `workspace`,
// reads inside `workspace` or `context_dir`.
//
// A relative path resolves against `workspace`, so a session's own directory is the
// default place to work and the shared context has to be named explicitly.
//
// The check is on the resolved path, not the spelling: absolute paths, `..` and symlinks
// all end up compared as canonical paths against canonical roots. That is the difference
// from validate_safe_path() in tools/tool_util.hpp, which only rejects the substring ".."
// and lets an absolute path through.
std::optional<std::string> resolve_in_workspace(const SessionWorkspace& scope,
                                                const std::string& path,
                                                WorkspaceAccess access);

// The scope for one session: `workspace_root/<session_store_key(id)>` paired with the
// shared context directory. The key comes from session_store_key() in memory.hpp, so a
// session's workspace and its memory store sit under the same component — one layout to
// reason about, and one place where a caller-supplied id is made safe as a path.
//
// An empty workspace_root, or an empty session id, yields no workspace: there is then
// nothing to be relative to and nothing writable, which is the personal-agent shape.
SessionWorkspace session_workspace(const std::string& workspace_root,
                                   const std::string& context_dir,
                                   const std::string& session_id);

// Deletes one session's workspace and everything in it, returning whether a directory was
// removed. A session that never wrote a file has none, which is an ordinary false.
//
// Only ever the single directory `session_workspace()` would hand that session: the root is
// shared by every session and survives, and a path that does not sit directly beneath it is
// refused rather than deleted. A symlink in that position is unlinked, not followed.
//
// Nothing is removed without both a root and an id, so the personal agent — which has
// neither — cannot delete anything through this call.
bool remove_session_workspace(const std::string& workspace_root,
                              const std::string& session_id);

} // namespace ptrclaw
