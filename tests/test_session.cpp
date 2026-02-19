#include <catch2/catch_test_macros.hpp>
#include "mock_http_client.hpp"
#include "session.hpp"

using namespace ptrclaw;

// SessionManager requires valid provider creation, so we need a config
// with a known provider. We use "ollama" since it doesn't need an API key.

static MockHttpClient test_http;

static Config make_test_config() {
    Config cfg;
    cfg.default_provider = "ollama";
    cfg.ollama_base_url = "http://localhost:11434";
    cfg.agent.max_tool_iterations = 5;
    cfg.agent.max_history_messages = 50;
    return cfg;
}

// ── SessionManager ──────────────────────────────────────────────

TEST_CASE("SessionManager: starts with no sessions", "[session]") {
    auto cfg = make_test_config();
    SessionManager mgr(cfg, test_http);
    REQUIRE(mgr.list_sessions().empty());
}

TEST_CASE("SessionManager: get_session creates new session", "[session]") {
    auto cfg = make_test_config();
    SessionManager mgr(cfg, test_http);
    mgr.get_session("sess1");
    auto sessions = mgr.list_sessions();
    REQUIRE(sessions.size() == 1);
    REQUIRE(sessions[0] == "sess1");
}

TEST_CASE("SessionManager: get_session returns same agent", "[session]") {
    auto cfg = make_test_config();
    SessionManager mgr(cfg, test_http);
    Agent& a1 = mgr.get_session("sess1");
    Agent& a2 = mgr.get_session("sess1");
    REQUIRE(&a1 == &a2);
}

TEST_CASE("SessionManager: multiple sessions", "[session]") {
    auto cfg = make_test_config();
    SessionManager mgr(cfg, test_http);
    mgr.get_session("a");
    mgr.get_session("b");
    mgr.get_session("c");
    REQUIRE(mgr.list_sessions().size() == 3);
}

TEST_CASE("SessionManager: remove_session deletes session", "[session]") {
    auto cfg = make_test_config();
    SessionManager mgr(cfg, test_http);
    mgr.get_session("sess1");
    mgr.get_session("sess2");
    mgr.remove_session("sess1");
    auto sessions = mgr.list_sessions();
    REQUIRE(sessions.size() == 1);
    REQUIRE(sessions[0] == "sess2");
}

TEST_CASE("SessionManager: remove_session on nonexistent is noop", "[session]") {
    auto cfg = make_test_config();
    SessionManager mgr(cfg, test_http);
    mgr.remove_session("does_not_exist");
    REQUIRE(mgr.list_sessions().empty());
}

TEST_CASE("SessionManager: evict_idle keeps recent sessions", "[session]") {
    auto cfg = make_test_config();
    SessionManager mgr(cfg, test_http);
    mgr.get_session("sess1");
    // With a large idle threshold, session should be kept
    mgr.evict_idle(999999);
    REQUIRE(mgr.list_sessions().size() == 1);
}
