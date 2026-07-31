#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include "mock_http_client.hpp"
#include "test_helpers.hpp"
#include "session.hpp"
#include "plugin.hpp"
#include "event.hpp"
#include "event_bus.hpp"

using namespace ptrclaw;
using json = nlohmann::json;

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

// Needs the OpenAI provider to be registered: "/auth <provider> <key>" checks the
// name against PluginRegistry, so with -Dwith_openai=false (plus openrouter and
// compatible, which imply it) this command answers "Unknown provider" and stores
// nothing. The two tests above do not need the guard — they assert the key is
// *unchanged*, and reach the refusal branch before any provider lookup.
#ifdef PTRCLAW_HAS_OPENAI
TEST_CASE("SessionManager: /auth openai <key> stores and persists the key", "[session]") {
    // The counterpart to the two tests above: the guard must refuse the
    // subcommands without breaking the legitimate form. This one reaches
    // persist_provider_key() -> modify_config_json(), so it runs under HomeGuard —
    // config code resolves "~" through $HOME, so the write lands in the temp dir
    // and never touches the developer's own config.
    HomeGuard home;
    home.write_default_config();

    auto cfg = make_test_config();
    auto reply = run_auth_command(cfg, "/auth openai sk-test-12345");

    REQUIRE(cfg.providers["openai"].api_key == "sk-test-12345");
    REQUIRE(reply.find("saved") != std::string::npos);
    // Asserting on the file, not just memory: persistence is the half that fails
    // silently and only shows up as a lost credential after a restart.
    auto persisted = home.read_config();
    REQUIRE(persisted["providers"]["openai"]["api_key"] == "sk-test-12345");
}
#endif // PTRCLAW_HAS_OPENAI

// ── Pushed conversation history ─────────────────────────────────
//
// A channel whose frontend owns the conversation sets ChannelMessage::history, and
// the session must hand exactly that window to the model.
//
// Both tests assert on the *outgoing provider payload* rather than on
// agent.history(), because "what the model was shown" is the property the feature
// exists for: a set_history() call that never reached a request would satisfy an
// agent.history() assertion while being useless.

static Config make_pushed_history_config() {
    Config cfg;
    cfg.provider = "anthropic";
    cfg.model = "claude-sonnet-4-6";
    cfg.providers["anthropic"].api_key = "test-key";
    // One post() per turn, so last_body unambiguously belongs to the turn asserted.
    cfg.agent.disable_streaming = true;
    cfg.agent.max_tool_iterations = 1;
    cfg.agent.max_history_messages = 50;
    // Keeps an injected [Memory context] block out of the user turn.
    cfg.memory.backend = "none";
    return cfg;
}

static const char* kStubReply =
    R"({"model":"claude-sonnet-4-6","content":[{"type":"text","text":"ok"}],)"
    R"("usage":{"input_tokens":10,"output_tokens":2}})";

TEST_CASE("SessionManager: a pushed history window reaches the provider in order",
          "[session]") {
    MockHttpClient http;
    http.next_response = {200, kStubReply};
    auto cfg = make_pushed_history_config();

    EventBus bus;
    SessionManager mgr(cfg, http);
    mgr.set_event_bus(&bus);
    mgr.subscribe_events();

    MessageReceivedEvent ev;
    ev.session_id = "pushed";
    ev.message.content = "and the second?";
    ev.message.history = std::vector<ChatMessage>{
        {Role::System,    "You are a hotel concierge.",   std::nullopt, std::nullopt},
        {Role::User,      "what was my first question?",  std::nullopt, std::nullopt},
        {Role::Assistant, "You asked about breakfast.",   std::nullopt, std::nullopt},
    };
    bus.publish(ev);

    REQUIRE(http.call_count == 1);
    auto body = json::parse(http.last_body);

    // A leading System message in the pushed window must become the system prompt,
    // which is how a caller supplies the agent's instructions per request. Anthropic
    // carries it out-of-band, so it is asserted there rather than in messages[].
    REQUIRE(body.contains("system"));
    REQUIRE(body["system"].get<std::string>().find("hotel concierge") != std::string::npos);

    // Roles and order both matter: collapsing prior turns into one user message is
    // the lossy shortcut this field exists to avoid.
    const auto& msgs = body["messages"];
    REQUIRE(msgs.size() == 3);
    REQUIRE(msgs[0]["role"] == "user");
    REQUIRE(msgs[0]["content"] == "what was my first question?");
    REQUIRE(msgs[1]["role"] == "assistant");
    REQUIRE(msgs[1]["content"] == "You asked about breakfast.");
    REQUIRE(msgs[2]["role"] == "user");
    REQUIRE(msgs[2]["content"] == "and the second?");
}

TEST_CASE("SessionManager: a pushed window replaces what the session accumulated",
          "[session]") {
    // The discriminating half. On a fresh session, "replace" and "append" are
    // indistinguishable — so run a turn first, then push a window that omits it.
    // An implementation that appended the pushed window to the session's own
    // history would pass the test above and fail this one.
    MockHttpClient http;
    http.next_response = {200, kStubReply};
    auto cfg = make_pushed_history_config();

    EventBus bus;
    SessionManager mgr(cfg, http);
    mgr.set_event_bus(&bus);
    mgr.subscribe_events();

    MessageReceivedEvent first;
    first.session_id = "pushed";
    first.message.content = "remember-me-please";
    bus.publish(first);
    REQUIRE(json::parse(http.last_body)["messages"][0]["content"] == "remember-me-please");

    MessageReceivedEvent second;
    second.session_id = "pushed";
    second.message.content = "a brand new topic";
    second.message.history = std::vector<ChatMessage>{
        {Role::User, "unrelated earlier turn", std::nullopt, std::nullopt},
    };
    bus.publish(second);

    auto body = json::parse(http.last_body);
    REQUIRE(http.last_body.find("remember-me-please") == std::string::npos);

    const auto& msgs = body["messages"];
    REQUIRE(msgs.size() == 2);
    REQUIRE(msgs[0]["content"] == "unrelated earlier turn");
    REQUIRE(msgs[1]["content"] == "a brand new topic");
}
