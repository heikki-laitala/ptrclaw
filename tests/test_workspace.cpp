#include <catch2/catch_test_macros.hpp>
#include "workspace.hpp"
#include "memory.hpp"
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

using namespace ptrclaw;

namespace {

// Real directories, because the whole point of the resolver is that it consults the
// filesystem: a symlink is only an escape if something resolves it.
struct WorkspaceFixture {
    std::filesystem::path root;
    std::filesystem::path workspace;
    std::filesystem::path context;
    std::filesystem::path outside;

    WorkspaceFixture() {
        root = std::filesystem::temp_directory_path() /
               ("ptrclaw_ws_" + std::to_string(getpid()) + "_" +
                std::to_string(counter()));
        std::filesystem::create_directories(root);
        // Canonical from here on: the temp dir is reached through a symlink on macOS
        // (/var → /private/var), and the resolver returns resolved paths by design, so
        // comparing against an unresolved expectation would fail for the wrong reason.
        root = std::filesystem::canonical(root);
        workspace = root / "sessions" / "abcd-sess";
        context = root / "context";
        outside = root / "elsewhere";
        std::filesystem::create_directories(workspace);
        std::filesystem::create_directories(context);
        std::filesystem::create_directories(outside);
        std::ofstream(context / "shared.txt") << "shared\n";
        std::ofstream(outside / "secret.txt") << "secret\n";
    }

    ~WorkspaceFixture() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    WorkspaceFixture(const WorkspaceFixture&) = delete;
    WorkspaceFixture& operator=(const WorkspaceFixture&) = delete;

    SessionWorkspace scope() const {
        return SessionWorkspace{workspace.string(), context.string()};
    }

    static int counter() {
        static int n = 0;
        return ++n;
    }
};

} // namespace

// ── relative paths ──────────────────────────────────────────────

TEST_CASE("resolve_in_workspace: a relative path lands in the session workspace",
          "[workspace]") {
    WorkspaceFixture fx;
    auto ws = fx.scope();

    auto write = resolve_in_workspace(ws, "notes.md", WorkspaceAccess::Write);
    REQUIRE(write.has_value());
    REQUIRE(write.value_or("") == (fx.workspace / "notes.md").string());

    // Relative reads resolve the same way, so a model that writes then reads its own file
    // finds it without knowing any absolute path.
    auto read = resolve_in_workspace(ws, "notes.md", WorkspaceAccess::Read);
    REQUIRE(read.has_value());
    REQUIRE(read.value_or("") == (fx.workspace / "notes.md").string());
}

TEST_CASE("resolve_in_workspace: a nested relative path is allowed", "[workspace]") {
    WorkspaceFixture fx;
    auto path = resolve_in_workspace(fx.scope(), "out/report/final.md",
                                     WorkspaceAccess::Write);
    REQUIRE(path.has_value());
    REQUIRE(path.value_or("") == (fx.workspace / "out/report/final.md").string());
}

// ── the shared context directory ────────────────────────────────

TEST_CASE("resolve_in_workspace: the shared context is readable", "[workspace]") {
    WorkspaceFixture fx;
    auto path = resolve_in_workspace(fx.scope(), (fx.context / "shared.txt").string(),
                                     WorkspaceAccess::Read);
    REQUIRE(path.has_value());
}

// Read-only is the whole reason two concurrent tasks cannot clobber the context they
// both work from, so a write aimed at it must fail rather than be redirected.
TEST_CASE("resolve_in_workspace: the shared context is not writable", "[workspace]") {
    WorkspaceFixture fx;
    auto path = resolve_in_workspace(fx.scope(), (fx.context / "shared.txt").string(),
                                     WorkspaceAccess::Write);
    REQUIRE_FALSE(path.has_value());
}

TEST_CASE("resolve_in_workspace: no context configured means only the workspace",
          "[workspace]") {
    WorkspaceFixture fx;
    SessionWorkspace ws{fx.workspace.string(), ""};

    REQUIRE_FALSE(resolve_in_workspace(ws, (fx.context / "shared.txt").string(),
                                       WorkspaceAccess::Read).has_value());
    REQUIRE(resolve_in_workspace(ws, "mine.txt", WorkspaceAccess::Write).has_value());
}

// ── escapes ─────────────────────────────────────────────────────

TEST_CASE("resolve_in_workspace: an absolute path outside every root is refused",
          "[workspace]") {
    WorkspaceFixture fx;
    auto ws = fx.scope();
    auto secret = (fx.outside / "secret.txt").string();

    REQUIRE_FALSE(resolve_in_workspace(ws, secret, WorkspaceAccess::Read).has_value());
    REQUIRE_FALSE(resolve_in_workspace(ws, secret, WorkspaceAccess::Write).has_value());
    // The case that matters most in a pod: the process's own credentials.
    REQUIRE_FALSE(resolve_in_workspace(ws, "/etc/passwd",
                                       WorkspaceAccess::Read).has_value());
}

TEST_CASE("resolve_in_workspace: traversal out of the workspace is refused",
          "[workspace]") {
    WorkspaceFixture fx;
    auto ws = fx.scope();

    REQUIRE_FALSE(resolve_in_workspace(ws, "../../elsewhere/secret.txt",
                                       WorkspaceAccess::Read).has_value());
    REQUIRE_FALSE(resolve_in_workspace(ws, "sub/../../../elsewhere/x",
                                       WorkspaceAccess::Write).has_value());
}

// Traversal that stays inside is fine — refusing it would be the substring check all over
// again, which is what this resolver replaces.
TEST_CASE("resolve_in_workspace: traversal that stays inside is allowed", "[workspace]") {
    WorkspaceFixture fx;
    auto path = resolve_in_workspace(fx.scope(), "sub/../notes.md",
                                     WorkspaceAccess::Write);
    REQUIRE(path.has_value());
    REQUIRE(path.value_or("") == (fx.workspace / "notes.md").string());
}

// A prefix comparison on strings would accept this: "/…/sessions/abcd-sess-evil" starts
// with "/…/sessions/abcd-sess".
TEST_CASE("resolve_in_workspace: a sibling sharing the root's prefix is refused",
          "[workspace]") {
    WorkspaceFixture fx;
    auto sibling = fx.workspace.string() + "-evil";
    std::filesystem::create_directories(sibling);

    REQUIRE_FALSE(resolve_in_workspace(fx.scope(), sibling + "/x.txt",
                                       WorkspaceAccess::Write).has_value());
}

TEST_CASE("resolve_in_workspace: a symlink pointing out is refused", "[workspace]") {
    WorkspaceFixture fx;
    std::error_code ec;
    std::filesystem::create_directory_symlink(fx.outside, fx.workspace / "escape", ec);
    if (ec) SKIP("symlinks unavailable on this filesystem");

    // Lexically this is inside the workspace; only resolving the link reveals it is not.
    REQUIRE_FALSE(resolve_in_workspace(fx.scope(), "escape/secret.txt",
                                       WorkspaceAccess::Read).has_value());
}

// ── degenerate input ────────────────────────────────────────────

TEST_CASE("resolve_in_workspace: no workspace means nothing is writable", "[workspace]") {
    WorkspaceFixture fx;
    SessionWorkspace ws{"", fx.context.string()};

    REQUIRE_FALSE(resolve_in_workspace(ws, "notes.md", WorkspaceAccess::Write).has_value());
    // Reads still work for the shared context, which is the read-only-pod case.
    REQUIRE(resolve_in_workspace(ws, (fx.context / "shared.txt").string(),
                                 WorkspaceAccess::Read).has_value());
}

TEST_CASE("resolve_in_workspace: an unscoped session resolves nothing", "[workspace]") {
    // Both roots empty is the personal-agent shape: the scoped tools are not compiled in,
    // and if they somehow were they would refuse everything rather than fall back to the
    // process cwd.
    SessionWorkspace ws{"", ""};
    REQUIRE_FALSE(resolve_in_workspace(ws, "notes.md", WorkspaceAccess::Read).has_value());
    REQUIRE_FALSE(resolve_in_workspace(ws, "/tmp/x", WorkspaceAccess::Write).has_value());
}

TEST_CASE("resolve_in_workspace: an empty path is refused", "[workspace]") {
    WorkspaceFixture fx;
    REQUIRE_FALSE(resolve_in_workspace(fx.scope(), "", WorkspaceAccess::Read).has_value());
}

// ── session_workspace ───────────────────────────────────────────

TEST_CASE("session_workspace: derives a directory under the root", "[workspace]") {
    auto scope = session_workspace("/work/sessions", "/work/context", "task-42");

    REQUIRE(scope.context_dir == "/work/context");
    REQUIRE(scope.workspace.rfind("/work/sessions/", 0) == 0);
    // The id appears sanitised, and the hash prefix keeps two ids that sanitise alike
    // apart — the same boundary the memory stores rely on.
    REQUIRE(scope.workspace.find("task-42") != std::string::npos);
    REQUIRE(scope.workspace.find(session_store_key("task-42")) != std::string::npos);
}

// One layout, one place where a caller-supplied id becomes a path component.
TEST_CASE("session_workspace: shares its key with the memory store", "[workspace]") {
    auto scope = session_workspace("/work/sessions", "", "task-42");
    auto store = session_store_path("/home/u/.ptrclaw/memory.json", "task-42");

    auto key = session_store_key("task-42");
    REQUIRE(scope.workspace.find(key) != std::string::npos);
    REQUIRE(store.find(key) != std::string::npos);
}

// A caller picks the session id, so it is a path-traversal boundary. Traversal needs a
// component that IS "..", which the hash prefix makes impossible, and a separator, which
// the substitution removes — a literal ".." inside the component is just an odd directory
// name and harmless.
TEST_CASE("session_workspace: an id that needs sanitising cannot escape", "[workspace]") {
    auto scope = session_workspace("/work/sessions", "", "../../etc/passwd");

    REQUIRE(scope.workspace.rfind("/work/sessions/", 0) == 0);

    std::filesystem::path path(scope.workspace);
    auto component = path.filename().string();
    REQUIRE(component != "..");
    REQUIRE(component != ".");
    // One component: every separator in the id was substituted away, so nothing below
    // the root can be addressed by naming a session.
    REQUIRE(component.find('/') == std::string::npos);
    REQUIRE(path.parent_path() == std::filesystem::path("/work/sessions"));
    // And it still resolves to a directory under the root rather than above it.
    REQUIRE(path.lexically_normal().string().rfind("/work/sessions/", 0) == 0);
}

TEST_CASE("session_workspace: no root or no id means no workspace", "[workspace]") {
    REQUIRE(session_workspace("", "/work/context", "task-42").workspace.empty());
    REQUIRE(session_workspace("/work/sessions", "/work/context", "").workspace.empty());
    // The shared context survives either way: a read-only pod is a legitimate shape.
    REQUIRE(session_workspace("", "/work/context", "").context_dir == "/work/context");
}
