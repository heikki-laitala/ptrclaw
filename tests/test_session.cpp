#include <catch2/catch_test_macros.hpp>
#include "mock_http_client.hpp"
#include "session.hpp"
#include "plugin.hpp"
#include "event.hpp"
#include "event_bus.hpp"

using namespace ptrclaw;

// SessionManager requires valid provider creation, so we pick the first
// registered provider (any will do — the HTTP client is mocked).

static MockHttpClient test_http;

static Config make_test_config() {
    Config cfg;
    cfg.provider = "anthropic";
    cfg.providers["anthropic"].api_key = "test-key";
    cfg.providers["openai"].api_key = "test-key";
    cfg.providers["openrouter"].api_key = "test-key";
    cfg.providers["ollama"].base_url = "http://localhost:11434";
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

// ── /auth argument handling ──────────────────────────────────────

// "start" and "finish" are OAuth subcommands, never credentials. The generic
// "/auth <provider> <api_key>" branch would otherwise store one of them as the
// key and persist it to config.json, silently destroying a working credential.
// The assertion holds in both build configurations: with the OAuth flow built the
// subcommand starts the flow, and without it the command is refused — neither may
// touch api_key.
// Driven through the public route — bus in, reply event out — rather than the
// private handler, so this exercises the path a real channel takes.
static std::string run_auth_command(Config& cfg, const std::string& line) {
    EventBus bus;
    SessionManager mgr(cfg, test_http);
    mgr.set_event_bus(&bus);
    mgr.subscribe_events();

    std::string reply;
    subscribe<MessageReadyEvent>(
        bus, [&reply](const MessageReadyEvent& e) { reply = e.content; });

    MessageReceivedEvent ev;
    ev.session_id = "auth_sess";
    ev.message.content = line;
    bus.publish(ev);
    return reply;
}

TEST_CASE("SessionManager: /auth openai start is never stored as the API key", "[session]") {
    auto cfg = make_test_config();
    auto reply = run_auth_command(cfg, "/auth openai start");
    REQUIRE(cfg.providers["openai"].api_key == "test-key");
    REQUIRE_FALSE(reply.empty());
}

TEST_CASE("SessionManager: /auth openai finish is never stored as the API key", "[session]") {
    auto cfg = make_test_config();
    auto reply = run_auth_command(cfg, "/auth openai finish some-callback-url");
    REQUIRE(cfg.providers["openai"].api_key == "test-key");
    REQUIRE_FALSE(reply.empty());
}

// The positive case — "/auth openai <api_key>" actually storing the key — is
// deliberately NOT tested here: that path calls persist_provider_key(), which goes
// through modify_config_json() and rewrites the developer's real
// ~/.ptrclaw/config.json. Running the suite must not overwrite someone's
// credentials. Covering it needs an injectable config path first.
