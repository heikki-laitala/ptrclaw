#include "workspace.hpp"
#include "memory.hpp"

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

    for (const auto& part : path.relative_path()) {
        if (part == ".") continue;
        if (part == "..") {
            out = out.parent_path();
            if (out.empty()) out = path.root_path();
            continue;
        }
        out /= part;

        // A chain of links, each of which may be relative to the link's own directory.
        while (fs::is_symlink(fs::symlink_status(out, ec)) && !ec) {
            if (--budget <= 0) return std::nullopt;
            fs::path target = fs::read_symlink(out, ec);
            if (ec) return std::nullopt;
            out = target.is_absolute() ? target : out.parent_path() / target;
            out = out.lexically_normal();
        }
        if (ec) ec.clear();  // symlink_status on a missing path is not an error here
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

} // namespace ptrclaw
