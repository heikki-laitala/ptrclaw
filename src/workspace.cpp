#include "workspace.hpp"
#include "memory.hpp"

#include <deque>
#include <filesystem>

namespace ptrclaw {

namespace {

namespace fs = std::filesystem;

// Whether `candidate` is inside `root`, both already canonical.
//
// lexically_relative rather than a string prefix: "/work/ws-evil" starts with "/work/ws"
// as text but is a sibling, and a prefix test would accept it. A relative path that stays
// inside never begins with "..", which is the whole check.
bool inside(const fs::path& candidate, const fs::path& root) {
    if (root.empty()) return false;
    auto rel = candidate.lexically_relative(root);
    if (rel.empty()) return false;
    if (rel == "..") return false;
    return rel.native().rfind("../", 0) != 0;
}

// Resolves a path the way the kernel will when the file is opened, including symlinks whose
// target does not exist yet.
//
// weakly_canonical() is not enough on its own: it canonicalises only the longest existing
// prefix, and a dangling symlink does not "exist" (exists() follows the link), so the link
// component survives untouched and looks like an ordinary file inside the root. ofstream
// then follows it and creates the target outside — which is the escape this walk closes.
//
// Each component is expanded with lstat semantics, so links are seen rather than followed,
// and the budget bounds a symlink loop instead of spinning on it.
std::optional<fs::path> resolve_symlinks(const fs::path& path) {
    std::error_code ec;
    fs::path out = path.root_path();
    int budget = 64;

    // A worklist rather than a single pass over the original components: a link target is
    // itself a path that may traverse further links. Substituting "dirlink/missing" and then
    // only testing that whole path would never examine `dirlink`, so a link to a dangling
    // path *through* another link would resolve to something that looks inside the root
    // while a write followed `dirlink` out of it.
    std::deque<fs::path> pending;
    for (const auto& part : path.relative_path()) pending.push_back(part);

    while (!pending.empty()) {
        fs::path part = pending.front();
        pending.pop_front();

        if (part == ".") continue;
        if (part == "..") {
            out = out.parent_path();
            if (out.empty()) out = path.root_path();
            continue;
        }

        fs::path candidate = out / part;
        if (fs::is_symlink(fs::symlink_status(candidate, ec)) && !ec) {
            if (--budget <= 0) return std::nullopt;   // a loop, or a chain past all reason
            fs::path target = fs::read_symlink(candidate, ec);
            if (ec) return std::nullopt;

            // An absolute target restarts from its own root; a relative one continues from
            // the link's directory, which is where `out` already points. Either way the
            // target's components go back on the worklist so each is examined in turn.
            if (target.is_absolute()) out = target.root_path();
            std::deque<fs::path> expanded;
            for (const auto& piece : target.relative_path()) expanded.push_back(piece);
            pending.insert(pending.begin(), expanded.begin(), expanded.end());
            continue;
        }
        if (ec) ec.clear();  // symlink_status on a missing path is not an error here

        out = candidate;
    }

    return out.lexically_normal();
}

// The root itself may be reached through symlinks (/var → /private/var on macOS), so it is
// canonicalised the ordinary way — it exists, or there is nothing to compare against.
std::optional<fs::path> canonical_or_none(const fs::path& path) {
    std::error_code ec;
    fs::path resolved = fs::weakly_canonical(path, ec);
    if (ec) return std::nullopt;
    return resolved;
}

} // namespace

std::optional<std::string> resolve_in_workspace(const SessionWorkspace& scope,
                                                const std::string& path,
                                                WorkspaceAccess access) {
    if (path.empty()) return std::nullopt;
    // Nothing is reachable without a root. Refusing beats falling back to the process
    // cwd, which is where the credentials and the rest of the pod live.
    if (scope.workspace.empty() && scope.context_dir.empty()) return std::nullopt;

    fs::path requested(path);
    // A relative path belongs to the session's own directory; with no workspace there is
    // nothing for it to be relative to.
    if (requested.is_relative()) {
        if (scope.workspace.empty()) return std::nullopt;
        requested = fs::path(scope.workspace) / requested;
    }

    auto candidate = resolve_symlinks(requested);
    if (!candidate) return std::nullopt;

    if (!scope.workspace.empty()) {
        auto root = canonical_or_none(scope.workspace);
        if (root && inside(*candidate, *root)) return candidate->string();
    }

    // The shared context is read-only, so a write that resolves into it is refused rather
    // than redirected: the model should see the failure, not silently write elsewhere.
    if (access == WorkspaceAccess::Read && !scope.context_dir.empty()) {
        auto root = canonical_or_none(scope.context_dir);
        if (!root || !inside(*candidate, *root)) return std::nullopt;

        // Inside the shared context — but the roots may overlap, and a path that is also
        // under workspace_root belongs to some session rather than to everyone. This
        // session's own directory was already accepted above, so anything still under the
        // root here is a sibling's.
        if (!scope.workspace_root.empty()) {
            auto sessions = canonical_or_none(scope.workspace_root);
            if (sessions && inside(*candidate, *sessions)) return std::nullopt;
        }
        return candidate->string();
    }

    return std::nullopt;
}

SessionWorkspace session_workspace(const std::string& workspace_root,
                                   const std::string& context_dir,
                                   const std::string& session_id) {
    SessionWorkspace scope;
    scope.context_dir = context_dir;
    scope.workspace_root = workspace_root;
    if (workspace_root.empty() || session_id.empty()) return scope;

    // The directory is not created here: a session that never touches a file should not
    // leave one behind, and the write tool creates parents on demand anyway.
    scope.workspace =
        (fs::path(workspace_root) / session_store_key(session_id)).string();
    return scope;
}

bool remove_session_workspace(const std::string& workspace_root,
                              const std::string& session_id) {
    // Empty either way means there is no per-session directory — the personal-agent shape.
    // Deleting nothing is the only safe reading: a relative fallback would resolve against
    // the process's cwd, which is the pod itself.
    if (workspace_root.empty() || session_id.empty()) return false;

    auto scope = session_workspace(workspace_root, "", session_id);
    if (scope.workspace.empty()) return false;

    fs::path target(scope.workspace);
    std::error_code ec;

    // The root has to exist for there to be anything under it, and canonicalising it also
    // resolves the symlinks a temp directory is reached through (/var → /private/var).
    fs::path root = fs::canonical(workspace_root, ec);
    if (ec) return false;

    // Defence in depth. session_store_key() already guarantees one component that is
    // neither "." nor "..", so this cannot trigger today — but the call deletes a tree
    // recursively, and a later change to the key derivation must fail closed rather than
    // widen what remove_all() is pointed at.
    if (target.parent_path().lexically_normal() != root ||
        !inside(target.lexically_normal(), root)) {
        return false;
    }

    // symlink_status, not status: if the workspace is a symlink, remove_all() unlinks the
    // link itself rather than following it, and reporting on the link is what happened.
    // exists() would follow it and answer about a tree outside the root.
    auto info = fs::symlink_status(target, ec);
    if (ec || !fs::exists(info)) return false;

    fs::remove_all(target, ec);
    return !ec;
}

} // namespace ptrclaw
