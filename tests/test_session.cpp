#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include "mock_http_client.hpp"
#include "test_helpers.hpp"
#include "session.hpp"
#include "plugin.hpp"
#include "event.hpp"
#include "event_bus.hpp"
#ifdef PTRCLAW_HAS_OPENAI_OAUTH
#include "providers/oauth_openai.hpp"
#endif

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
// Defaults to CLI origin: /auth writes the shared Config and the config file, so it is
// refused anywhere else — most of the tests below are about the guard inside the command,
// which only a CLI-origin message reaches.
//
// `from_cli` is what the guard keys off, never the session id: a channel copies the
// session id out of the request, so an id-based check would be spoofable. The two vary
// independently here on purpose, so a caller can be made to *claim* the CLI's id without
// having its origin.
static std::string run_auth_command(
    Config& cfg, const std::string& line, bool from_cli = true,
    const std::string& session_id = SessionManager::kCliSessionId) {
    EventBus bus;
    SessionManager mgr(cfg, test_http);
    mgr.set_event_bus(&bus);
    mgr.subscribe_events();

    std::string reply;
    subscribe<MessageReadyEvent>(
        bus, [&reply](const MessageReadyEvent& e) { reply = e.content; });

    MessageReceivedEvent ev;
    ev.session_id = session_id;
    ev.from_cli = from_cli;
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

// The reply claims the change was saved, so it has to be: apply_oauth_result only writes
// the token fields, and without the selection the next start comes back on the old
// provider and model. Driven from a CLI-originating message, which is the only kind /auth
// answers now.
#ifdef PTRCLAW_HAS_OPENAI_OAUTH
TEST_CASE("SessionManager: /auth openai finish persists provider and model", "[session]") {
    HomeGuard home;
    home.write_default_config();

    auto cfg = make_test_config();   // provider "anthropic"
    cfg.allow_channel_commands = true;
    MockHttpClient http;
    EventBus bus;
    SessionManager mgr(cfg, http);
    mgr.set_event_bus(&bus);
    mgr.subscribe_events();

    std::string reply;
    subscribe<MessageReadyEvent>(
        bus, [&reply](const MessageReadyEvent& e) { reply = e.content; });

    MessageReceivedEvent start;
    start.session_id = "oauth_sess";
    start.from_cli = true;   // /auth is CLI-only; see the gate in handle_message
    start.message.content = "/auth openai start";
    bus.publish(start);

    http.next_response = {200,
        R"({"access_token": "tok", "refresh_token": "ref", "expires_in": 3600})"};

    MessageReceivedEvent finish;
    finish.session_id = "oauth_sess";
    finish.from_cli = true;
    finish.message.content = "/auth openai finish test-code";
    bus.publish(finish);

    REQUIRE(reply.find("connected") != std::string::npos);
    REQUIRE(cfg.provider == "openai");
    // Was on anthropic's model, which the subscription cannot serve.
    REQUIRE(cfg.model == std::string(kDefaultOAuthModel));

    auto persisted = home.read_config();
    REQUIRE(persisted["provider"] == "openai");
    REQUIRE(persisted["model"] == std::string(kDefaultOAuthModel));
}

// Connecting must not hand the ChatGPT backend a model the configuration says the
// subscription cannot serve. oauth_models can exclude even the default, so the credential
// for whatever model is chosen has to be resolved the same way every other switch is.
TEST_CASE("SessionManager: /auth openai finish honours oauth_models for the default",
          "[session]") {
    HomeGuard home;
    home.write_default_config();

    auto cfg = make_test_config();
    cfg.allow_channel_commands = true;
    cfg.providers["openai"].oauth_models = {"gpt-4o"};   // excludes kDefaultOAuthModel
    MockHttpClient http;
    EventBus bus;
    SessionManager mgr(cfg, http);
    mgr.set_event_bus(&bus);
    mgr.subscribe_events();

    std::string reply;
    subscribe<MessageReadyEvent>(
        bus, [&reply](const MessageReadyEvent& e) { reply = e.content; });

    MessageReceivedEvent start;
    start.session_id = "oauth_sess";
    start.from_cli = true;   // /auth is CLI-only; see the gate in handle_message
    start.message.content = "/auth openai start";
    bus.publish(start);

    http.next_response = {200,
        R"({"access_token": "tok", "refresh_token": "ref", "expires_in": 3600})"};

    MessageReceivedEvent finish;
    finish.session_id = "oauth_sess";
    finish.from_cli = true;
    finish.message.content = "/auth openai finish test-code";
    bus.publish(finish);

    // An API key is configured, so the model is usable — over that key, not the
    // subscription. Saying so beats letting the next turn look like OAuth traffic.
    REQUIRE(reply.find("oauth_models") != std::string::npos);
    REQUIRE(cfg.model == std::string(kDefaultOAuthModel));
    REQUIRE(cfg.providers["openai"].oauth_access_token == "tok");
}

TEST_CASE("SessionManager: /auth openai finish reports a model no credential can serve",
          "[session]") {
    HomeGuard home;
    home.write_default_config();

    auto cfg = make_test_config();
    cfg.allow_channel_commands = true;
    cfg.providers["openai"].api_key.clear();            // OAuth is the only credential
    cfg.providers["openai"].oauth_models = {"gpt-4o"};  // ...and it excludes the default
    MockHttpClient http;
    EventBus bus;
    SessionManager mgr(cfg, http);
    mgr.set_event_bus(&bus);
    mgr.subscribe_events();

    std::string reply;
    subscribe<MessageReadyEvent>(
        bus, [&reply](const MessageReadyEvent& e) { reply = e.content; });

    MessageReceivedEvent start;
    start.session_id = "oauth_sess";
    start.from_cli = true;   // /auth is CLI-only; see the gate in handle_message
    start.message.content = "/auth openai start";
    bus.publish(start);

    http.next_response = {200,
        R"({"access_token": "tok", "refresh_token": "ref", "expires_in": 3600})"};

    MessageReceivedEvent finish;
    finish.session_id = "oauth_sess";
    finish.from_cli = true;
    finish.message.content = "/auth openai finish test-code";
    bus.publish(finish);

    // The tokens are stored either way — losing them because the model selection failed
    // would send the user back through the whole browser flow.
    REQUIRE(cfg.providers["openai"].oauth_access_token == "tok");
    REQUIRE(reply.find("saved") != std::string::npos);
    REQUIRE(reply.find(kDefaultOAuthModel) != std::string::npos);
    REQUIRE(reply.find("No API key") != std::string::npos);
}
#endif // PTRCLAW_HAS_OPENAI_OAUTH
TEST_CASE("SessionManager: /auth is refused on a channel session", "[session]") {
    // The credential goes into the process-wide Config and into
    // ~/.ptrclaw/config.json, for every session — so a remote caller on a channel
    // must not be able to set it. HomeGuard anyway, so a regression writes to the
    // temp dir rather than the developer's real config.
    HomeGuard home;
    home.write_default_config();

    auto cfg = make_test_config();
    // Commands opted ON, deliberately: with them off the outer gate refuses every
    // command and this would pass without /auth's own guard existing at all. What is
    // under test is that opting a channel into commands still does not buy /auth.
    cfg.allow_channel_commands = true;
    // An ordinary channel id here; the test below does the same with the CLI's own id.
    auto reply = run_auth_command(cfg, "/auth openai sk-should-not-stick",
                                  /*from_cli=*/false, "telegram_12345");

    REQUIRE(cfg.providers["openai"].api_key == "test-key");
    REQUIRE(reply.find("only available on the local CLI") != std::string::npos);

    auto persisted = home.read_config();
    REQUIRE(persisted["providers"]["openai"]["api_key"] != "sk-should-not-stick");
}

TEST_CASE("SessionManager: a channel caller claiming session id 'cli' is still refused",
          "[session]") {
    // The HTTP channel copies `session` verbatim out of the request body, so a
    // guard keyed on the session id would let any caller POST
    // {"session":"cli","message":"/auth ..."} and set credentials for the whole
    // process. The guard keys off MessageReceivedEvent::from_cli, which nothing that
    // reads a socket sets.
    //
    // Same id as the CLI test above, opposite expectation — that pairing is the point.
    HomeGuard home;
    home.write_default_config();

    auto cfg = make_test_config();
    // Opted on, as in the test above: otherwise the outer gate refuses this before
    // /auth's own guard is reached, and the assertion below would pass vacuously.
    cfg.allow_channel_commands = true;
    auto reply = run_auth_command(cfg, "/auth openai sk-spoofed",
                                  /*from_cli=*/false,
                                  SessionManager::kCliSessionId);

    REQUIRE(cfg.providers["openai"].api_key == "test-key");
    REQUIRE(reply.find("only available on the local CLI") != std::string::npos);

    auto persisted = home.read_config();
    REQUIRE(persisted["providers"]["openai"]["api_key"] != "sk-spoofed");
}
#endif // PTRCLAW_HAS_OPENAI

TEST_CASE("SessionManager: a failing turn answers rather than hanging",
          "[session]") {
    // get_session() throws when the provider cannot be built. Inline on the poll
    // loop that reached main()'s handler and exited, dropping the caller's
    // connection. On a worker thread TurnPool catches it, so without a reply the
    // caller waits out the channel's turn timeout — 120s for HTTP by default.
    Config cfg;
    cfg.provider = "definitely-not-a-registered-provider";

    EventBus bus;
    SessionManager mgr(cfg, test_http);
    mgr.set_event_bus(&bus);
    mgr.subscribe_events();

    std::string reply;
    bool got_reply = false;
    subscribe<MessageReadyEvent>(bus, [&](const MessageReadyEvent& e) {
        reply = e.content;
        got_reply = true;
    });

    MessageReceivedEvent ev;
    ev.session_id = "doomed";
    ev.message.reply_target = "doomed";
    ev.message.content = "hello";
    REQUIRE_NOTHROW(bus.publish(ev));  // must not escape to the worker

    REQUIRE(got_reply);
    REQUIRE(reply.find("failed") != std::string::npos);
}

TEST_CASE("SessionManager: a failing local turn still propagates", "[session]") {
    // The exit code is the only failure signal `ptrclaw -m` and pipe mode have.
    // Turning a missing API key into a reply would make them print an apology to
    // stdout and exit 0, which a cron job or CI step reads as the model's answer.
    Config cfg;
    cfg.provider = "definitely-not-a-registered-provider";

    EventBus bus;
    SessionManager mgr(cfg, test_http);
    mgr.set_event_bus(&bus);
    mgr.subscribe_events();

    MessageReceivedEvent ev;
    ev.session_id = SessionManager::kCliSessionId;
    ev.message.content = "hello";
    ev.from_cli = true;
    REQUIRE_THROWS(bus.publish(ev));
}

TEST_CASE("SessionManager: a new session knows its id before it subscribes",
          "[session]") {
    // Agent's event handlers accept anything while session_id_ is empty, so an
    // Agent that subscribed before its id was set would, with workers > 1, take a
    // concurrently-created session's ToolsAvailableEvent. Observed through the
    // ordering that guarantees it: the specs for a new session must arrive after
    // that session exists, never against a half-built one.
    auto cfg = make_test_config();

    EventBus bus;
    SessionManager mgr(cfg, test_http);
    mgr.set_event_bus(&bus);

    std::vector<std::string> spec_sessions;
    subscribe<ToolsAvailableEvent>(bus, [&](const ToolsAvailableEvent& e) {
        spec_sessions.push_back(e.session_id);
    });

    mgr.get_session("alice");
    mgr.get_session("bob");

    REQUIRE(spec_sessions.size() == 2);
    REQUIRE(spec_sessions[0] == "alice");
    REQUIRE(spec_sessions[1] == "bob");
}

// ── /model and /provider scope ──────────────────────────────────
//
// The Config is shared by every session and turns run on several threads, so a
// channel session may change only its own Agent. Writing the shared Config from
// there is both a data race and a cross-tenant change.

namespace {

// Runs `line` for `session_id` and returns the reply.
//
// `from_cli` decides whether the shared Config may be written, and is passed explicitly
// rather than derived from the session id — the id is a routing key the caller picks
// (see the bypass covered further down). Callers that pass false must also opt commands
// on, or the outer gate refuses the command before this scoping is reached.
std::string run_command(Config& cfg, const std::string& line,
                        const std::string& session_id, bool from_cli = false) {
    EventBus bus;
    SessionManager mgr(cfg, test_http);
    mgr.set_event_bus(&bus);
    mgr.subscribe_events();

    std::string reply;
    subscribe<MessageReadyEvent>(
        bus, [&reply](const MessageReadyEvent& e) { reply = e.content; });

    MessageReceivedEvent ev;
    ev.session_id = session_id;
    ev.from_cli = from_cli;
    ev.message.content = line;
    bus.publish(ev);
    return reply;
}

} // namespace

TEST_CASE("SessionManager: /model on a channel session does not touch Config",
          "[session]") {
    HomeGuard home;
    home.write_default_config();

    auto cfg = make_test_config();
    // Commands opted on, so the command actually runs and the assertion below is about
    // its SCOPE. With them off the outer gate refuses it and this passes vacuously.
    cfg.allow_channel_commands = true;
    const std::string original = cfg.model;

    auto reply = run_command(cfg, "/model claude-haiku-4-5-20251001",
                             "telegram_12345", /*from_cli=*/false);

    REQUIRE(reply.find("Model set to") != std::string::npos);
    // And says so, rather than silently reverting when the session is evicted.
    REQUIRE(reply.find("this conversation only") != std::string::npos);
    REQUIRE(cfg.model == original);

    auto persisted = home.read_config();
    REQUIRE(persisted["model"] != "claude-haiku-4-5-20251001");
}

TEST_CASE("SessionManager: /model on the CLI session persists", "[session]") {
    HomeGuard home;
    home.write_default_config();

    auto cfg = make_test_config();
    auto reply = run_command(cfg, "/model claude-haiku-4-5-20251001",
                             SessionManager::kCliSessionId, /*from_cli=*/true);

    REQUIRE(reply.find("Model set to") != std::string::npos);
    REQUIRE(reply.find("this conversation only") == std::string::npos);
    REQUIRE(cfg.model == "claude-haiku-4-5-20251001");

    auto persisted = home.read_config();
    REQUIRE(persisted["model"] == "claude-haiku-4-5-20251001");
}

TEST_CASE("SessionManager: /model on one channel session leaves others alone",
          "[session]") {
    // The cross-tenant half: two sessions, one Config, one /model.
    auto cfg = make_test_config();
    cfg.allow_channel_commands = true;  // as above: so the command runs at all

    EventBus bus;
    SessionManager mgr(cfg, test_http);
    mgr.set_event_bus(&bus);
    mgr.subscribe_events();

    Agent& alice = mgr.get_session("alice");
    Agent& bob = mgr.get_session("bob");
    const std::string bob_model = bob.model();

    MessageReceivedEvent ev;
    ev.session_id = "alice";
    ev.message.content = "/model claude-haiku-4-5-20251001";
    bus.publish(ev);

    REQUIRE(alice.model() == "claude-haiku-4-5-20251001");
    REQUIRE(bob.model() == bob_model);
    REQUIRE(cfg.model != "claude-haiku-4-5-20251001");
}

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

// ── Channel command gating ──────────────────────────────────────
//
// Tested by what is REFUSED, not by what works: the property is that a visitor cannot
// reach the operator's command surface, and only a blocked command demonstrates it.
//
// The probe is the REPLY rather than a state change, because the obvious state probes
// are confounded. Sending a second message with no history to see whether /clear wiped
// it also triggers auto-hatch — which clears history too, for an entirely different
// reason — and setting memory.backend to "none" does not prevent that: NoneMemory is
// still an object, so agent.memory() is non-null and is_hatched() is false. A test
// built that way passes whether or not the gate works.

static Config command_test_config(bool allow_channel_commands) {
    auto cfg = make_test_config();
    cfg.allow_channel_commands = allow_channel_commands;
    return cfg;
}

// Sends one message and returns the reply. History is supplied (empty is enough, since
// PtrClaw checks the field's presence) so auto-hatch stays out of the way.
//
// `from_cli` is passed explicitly rather than derived from the session id, because that
// distinction is the thing under test.
static std::string reply_to(Config& cfg,
                            const std::string& session_id,
                            const std::string& line,
                            bool from_cli = false) {
    EventBus bus;
    SessionManager mgr(cfg, test_http);
    mgr.set_event_bus(&bus);
    mgr.subscribe_events();

    std::string reply;
    subscribe<MessageReadyEvent>(
        bus, [&reply](const MessageReadyEvent& e) { reply = e.content; });

    MessageReceivedEvent ev;
    ev.session_id = session_id;
    ev.from_cli = from_cli;
    ev.message.content = line;
    ev.message.history = std::vector<ChatMessage>{};
    bus.publish(ev);
    return reply;
}

TEST_CASE("SessionManager: a channel cannot run /status by default", "[session]") {
    // cmd_status reports the provider, the model and the token estimate — the agent's
    // internals, to whoever typed into a chat box.
    auto cfg = command_test_config(false);
    REQUIRE(reply_to(cfg, "visitor", "/status").find("Provider: ") == std::string::npos);
}

TEST_CASE("SessionManager: a channel cannot run /models by default", "[session]") {
    auto cfg = command_test_config(false);
    REQUIRE(reply_to(cfg, "visitor", "/models").find("Providers:") == std::string::npos);
}

TEST_CASE("SessionManager: allow_channel_commands re-enables them", "[session]") {
    // The control. Without it the two tests above would also pass if /status had simply
    // stopped working, or if the reply never arrived at all.
    auto cfg = command_test_config(true);
    REQUIRE(reply_to(cfg, "visitor", "/status").find("Provider: ") != std::string::npos);
}

TEST_CASE("SessionManager: the CLI keeps its commands regardless", "[session]") {
    // The operator already owns the shell the CLI runs in, so gating it there would
    // remove the command surface without protecting anything.
    auto cfg = command_test_config(false);
    auto reply = reply_to(cfg, SessionManager::kCliSessionId, "/status", /*from_cli=*/true);
    REQUIRE(reply.find("Provider: ") != std::string::npos);
}

TEST_CASE("SessionManager: a caller cannot claim the CLI by naming its session",
          "[session]") {
    // The bypass this gate was first written with. A channel message's session_id is
    // ChannelMessage::sender, and on the HTTP channel that is the caller's own `session`
    // field — so POSTing {"session":"cli","message":"/auth openai <key>"} reopened the
    // entire command surface while allow_channel_commands was false.
    //
    // Same session id as the test above, opposite expectation: the id is a routing key
    // chosen by whoever is speaking, and carries no trust.
    auto cfg = command_test_config(false);
    auto reply = reply_to(cfg, SessionManager::kCliSessionId, "/status", /*from_cli=*/false);
    REQUIRE(reply.find("Provider: ") == std::string::npos);
}

TEST_CASE("SessionManager: a blocked command is answered, not rejected", "[session]") {
    // It falls through to the agent as ordinary text. Refusing it instead would make an
    // ordinary message that happens to start with a slash fail for no visible reason.
    auto cfg = command_test_config(false);
    REQUIRE_FALSE(reply_to(cfg, "visitor", "/status").empty());
}
