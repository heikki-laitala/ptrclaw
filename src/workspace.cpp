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

// Resolves symlinks and `..` in whatever part of the path already exists, and appends the
// rest lexically — so a file about to be created still resolves, while an existing symlink
// pointing out of the root is followed and therefore caught by inside().
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

    auto candidate = canonical_or_none(requested);
    if (!candidate) return std::nullopt;

    if (!scope.workspace.empty()) {
        auto root = canonical_or_none(scope.workspace);
        if (root && inside(*candidate, *root)) return candidate->string();
    }

    // The shared context is read-only, so a write that resolves into it is refused rather
    // than redirected: the model should see the failure, not silently write elsewhere.
    if (access == WorkspaceAccess::Read && !scope.context_dir.empty()) {
        auto root = canonical_or_none(scope.context_dir);
        if (root && inside(*candidate, *root)) return candidate->string();
    }

    return std::nullopt;
}

SessionWorkspace session_workspace(const std::string& workspace_root,
                                   const std::string& context_dir,
                                   const std::string& session_id) {
    SessionWorkspace scope;
    scope.context_dir = context_dir;
    if (workspace_root.empty() || session_id.empty()) return scope;

    // The directory is not created here: a session that never touches a file should not
    // leave one behind, and the write tool creates parents on demand anyway.
    scope.workspace =
        (fs::path(workspace_root) / session_store_key(session_id)).string();
    return scope;
}

} // namespace ptrclaw
