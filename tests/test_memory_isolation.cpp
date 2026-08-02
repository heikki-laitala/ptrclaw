#include <catch2/catch_test_macros.hpp>
#include "memory.hpp"
#include "memory/response_cache.hpp"
#include "session.hpp"
#include "test_helpers.hpp"
#include "mock_http_client.hpp"
#include <chrono>
#include <filesystem>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace ptrclaw;

namespace {

MockHttpClient isolation_http;

Config make_config(const std::string& isolation, const std::string& path) {
    Config cfg;
    cfg.provider = "anthropic";
    cfg.providers["anthropic"].api_key = "test-key";
    cfg.memory.backend = "json";
    cfg.memory.isolation = isolation;
    cfg.memory.path = path;
    cfg.memory.synthesis = false;
    return cfg;
}

// The single path component session_store_path() derives for an id.
std::string derived_component(const std::string& session_id) {
    auto p = std::filesystem::path(
        session_store_path("/base/dir/memory.json", session_id));
    return p.parent_path().filename().string();
}

} // namespace

// ── session_store_path: the path-traversal boundary ─────────────

TEST_CASE("session_store_path: keeps the store under the sessions directory",
          "[memory][isolation]") {
    auto path = session_store_path("/base/dir/memory.json", "alice");
    auto p = std::filesystem::path(path);

    REQUIRE(p.filename() == "memory.json");
    REQUIRE(p.parent_path().parent_path() == "/base/dir/sessions");
}

TEST_CASE("session_store_path: hostile session ids cannot escape",
          "[memory][isolation]") {
    // The HTTP channel reads `session` straight from the request body, so these
    // are reachable inputs, not hypotheticals.
    const std::vector<std::string> hostile = {
        "../../etc/passwd",
        "..",
        ".",
        "/absolute/path",
        "a/b/c",
        "..\\..\\windows",
        std::string("nul\0byte", 8),
        std::string(4096, 'x'),
        "\xF0\x9F\x92\xA9 unicode",
        "",
    };

    for (const auto& id : hostile) {
        auto component = derived_component(id);

        INFO("session id: " << id);
        REQUIRE(component.find('/') == std::string::npos);
        REQUIRE(component.find('\\') == std::string::npos);
        REQUIRE(component != ".");
        REQUIRE(component != "..");
        // Never a dotfile, and never long enough to be a problem: 8 hex + '-' + 32.
        REQUIRE(component[0] != '.');
        REQUIRE(component.size() <= 41);

        // And the result really is inside the sessions directory.
        auto p = std::filesystem::path(
            session_store_path("/base/dir/memory.json", id));
        REQUIRE(p.lexically_normal().string().rfind("/base/dir/sessions/", 0) == 0);
    }
}

TEST_CASE("session_store_path: ids that sanitize alike stay distinct",
          "[memory][isolation]") {
    // "a/b" and "a:b" both sanitize to "a_b" — the hash prefix is what keeps two
    // callers from sharing a store.
    REQUIRE(derived_component("a/b") != derived_component("a:b"));
    REQUIRE(derived_component("../x") != derived_component("__/x"));

    // Distinct ids, distinct directories, at scale.
    std::set<std::string> seen;
    for (int i = 0; i < 500; ++i) {
        seen.insert(derived_component("session-" + std::to_string(i)));
    }
    REQUIRE(seen.size() == 500);
}

TEST_CASE("session_store_path: same id gives the same store every time",
          "[memory][isolation]") {
    REQUIRE(session_store_path("/base/memory.db", "alice") ==
            session_store_path("/base/memory.db", "alice"));
}

TEST_CASE("session_store_path: preserves the backend's filename",
          "[memory][isolation]") {
    auto p = std::filesystem::path(session_store_path("/base/memory.db", "bob"));
    REQUIRE(p.filename() == "memory.db");
}

// ── create_memory ───────────────────────────────────────────────

TEST_CASE("create_memory: shared isolation ignores the session id",
          "[memory][isolation]") {
    HomeGuard home;
    auto cfg = make_config("shared", (home.tmpdir / "mem.json").string());

    auto a = create_memory(cfg, "alice");
    auto b = create_memory(cfg, "bob");
    REQUIRE(a);
    REQUIRE(b);

    a->store("shared_key", "from alice", MemoryCategory::Knowledge, "alice");
    REQUIRE(create_memory(cfg, "bob")->get("shared_key").has_value());
}

TEST_CASE("create_memory: session isolation separates the stores",
          "[memory][isolation]") {
    HomeGuard home;
    auto cfg = make_config("session", (home.tmpdir / "mem.json").string());

    auto alice = create_memory(cfg, "alice");
    auto bob = create_memory(cfg, "bob");
    REQUIRE(alice);
    REQUIRE(bob);

    alice->store("user_name", "Alice", MemoryCategory::Core, "alice");

    // Bob cannot see it, by key or by recall.
    REQUIRE_FALSE(bob->get("user_name").has_value());
    REQUIRE(bob->recall("Alice", 5, std::nullopt).empty());
    REQUIRE(bob->count(std::nullopt) == 0);

    // And Bob writing the same key does not clobber Alice's value — the collision
    // the shared store cannot avoid, since store() upserts on a global key.
    bob->store("user_name", "Bob", MemoryCategory::Core, "bob");
    auto reread = create_memory(cfg, "alice");
    REQUIRE(reread->get("user_name").value_or(MemoryEntry{}).content == "Alice");
}

TEST_CASE("create_memory: session isolation with an empty id stays shared",
          "[memory][isolation]") {
    HomeGuard home;
    auto cfg = make_config("session", (home.tmpdir / "mem.json").string());

    create_memory(cfg, "")->store("k", "v", MemoryCategory::Knowledge, "");
    REQUIRE(std::filesystem::exists(home.tmpdir / "mem.json"));
}

// ── SessionManager wiring ───────────────────────────────────────

TEST_CASE("SessionManager: shared isolation gives every session one backend",
          "[memory][isolation][session]") {
    // Not just an optimisation. JsonMemory holds the whole document and rewrites
    // it wholesale, so two instances over one file lose each other's writes.
    HomeGuard home;
    auto cfg = make_config("shared", (home.tmpdir / "mem.json").string());
    SessionManager mgr(cfg, isolation_http);

    Memory* a = mgr.get_session("alice").memory();
    Memory* b = mgr.get_session("bob").memory();

    REQUIRE(a != nullptr);
    REQUIRE(a == b);
}

TEST_CASE("SessionManager: session isolation gives each session its own backend",
          "[memory][isolation][session]") {
    HomeGuard home;
    auto cfg = make_config("session", (home.tmpdir / "mem.json").string());
    SessionManager mgr(cfg, isolation_http);

    Memory* a = mgr.get_session("alice").memory();
    Memory* b = mgr.get_session("bob").memory();

    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    REQUIRE(a != b);

    a->store("secret", "alice only", MemoryCategory::Core, "alice");
    REQUIRE_FALSE(b->get("secret").has_value());

    // The same session keeps its backend across lookups.
    REQUIRE(mgr.get_session("alice").memory() == a);
}

TEST_CASE("SessionManager: shared isolation gives every session one response cache",
          "[memory][isolation][session]") {
    // Same reason as the memory store: ResponseCache rewrites its file whole, and
    // atomic_write_file derives the temp path from the target, so two instances
    // over one path can interleave into the same .tmp and corrupt it.
    HomeGuard home;
    auto cfg = make_config("shared", (home.tmpdir / "mem.json").string());
    cfg.memory.response_cache = true;
    SessionManager mgr(cfg, isolation_http);

    ResponseCache* a = mgr.get_session("alice").response_cache();
    ResponseCache* b = mgr.get_session("bob").response_cache();

    REQUIRE(a != nullptr);
    REQUIRE(a == b);
}

TEST_CASE("SessionManager: session isolation gives each session its own cache",
          "[memory][isolation][session]") {
    // A shared cache under isolation would hand session A's completion to B: the
    // key is model + system prompt + conversation, none of which is session-bound.
    HomeGuard home;
    auto cfg = make_config("session", (home.tmpdir / "mem.json").string());
    cfg.memory.response_cache = true;
    SessionManager mgr(cfg, isolation_http);

    ResponseCache* a = mgr.get_session("alice").response_cache();
    ResponseCache* b = mgr.get_session("bob").response_cache();

    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    REQUIRE(a != b);

    a->put("model", "prompt", "question", "alice's answer");
    REQUIRE_FALSE(b->get("model", "prompt", "question").has_value());
    REQUIRE(a->get("model", "prompt", "question").value_or("") ==
            "alice's answer");
}

TEST_CASE("SessionManager: no response cache when the feature is off",
          "[memory][isolation][session]") {
    HomeGuard home;
    auto cfg = make_config("session", (home.tmpdir / "mem.json").string());
    SessionManager mgr(cfg, isolation_http);

    REQUIRE(mgr.get_session("alice").response_cache() == nullptr);
}

TEST_CASE("SessionManager: removing a session drops its backend",
          "[memory][isolation][session]") {
    // The per-session backends are held in a map beside the sessions; if removal
    // missed it, a long-lived process would accumulate one open store per session
    // id it had ever seen.
    HomeGuard home;
    auto cfg = make_config("session", (home.tmpdir / "mem.json").string());
    SessionManager mgr(cfg, isolation_http);

    mgr.get_session("alice").memory()->store("k", "v", MemoryCategory::Core,
                                             "alice");

    mgr.remove_session("alice");
    REQUIRE(mgr.list_sessions().empty());

    // The store survived on disk, which is the other half: removal reclaims
    // memory, it does not forget.
    REQUIRE(mgr.get_session("alice").memory()->get("k").has_value());
}

TEST_CASE("SessionManager: idle eviction drops the session's backend",
          "[memory][isolation][session]") {
    // Same reclaim, via the path the poll loop actually uses. evict_idle compares
    // whole seconds and is strictly greater-than, so a session created in this
    // second is not idle yet — hence the wait.
    HomeGuard home;
    auto cfg = make_config("session", (home.tmpdir / "mem.json").string());
    SessionManager mgr(cfg, isolation_http);

    mgr.get_session("alice").memory()->store("k", "v", MemoryCategory::Core,
                                             "alice");

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    mgr.evict_idle(0);
    REQUIRE(mgr.list_sessions().empty());

    REQUIRE(mgr.get_session("alice").memory()->get("k").has_value());
}
