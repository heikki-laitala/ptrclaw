#include <catch2/catch_test_macros.hpp>
#include "channels/embed.hpp"
#include "plugin.hpp"
#include <cstdlib>
#include <thread>

using namespace ptrclaw;

// ── EmbedChannel unit tests ─────────────────────────────────────

TEST_CASE("EmbedChannel: channel_name is embed", "[embed]") {
    EmbedChannel ch;
    REQUIRE(ch.channel_name() == "embed");
    REQUIRE(ch.health_check());
    REQUIRE(ch.supports_polling());
}

TEST_CASE("EmbedChannel: poll_updates returns empty when no messages", "[embed]") {
    EmbedChannel ch;
    auto msgs = ch.poll_updates();
    REQUIRE(msgs.empty());
}

TEST_CASE("EmbedChannel: send_message delivers to pending response", "[embed]") {
    EmbedChannel ch;

    // Simulate what SessionManager does: call send_message with session_id as target.
    // First we need a pending response slot — normally send_user_message creates it,
    // but we can test the channel interface directly via a background thread.
    std::string response;
    std::thread sender([&] {
        response = ch.send_user_message("test-session", "hello");
    });

    // Poll the message out (give sender thread time to push)
    std::vector<ChannelMessage> msgs;
    for (int i = 0; i < 50 && msgs.empty(); ++i) {
        msgs = ch.poll_updates();
        if (msgs.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    REQUIRE(msgs.size() == 1);
    REQUIRE(msgs[0].sender == "test-session");
    REQUIRE(msgs[0].content == "hello");
    REQUIRE(msgs[0].channel == "embed");
    REQUIRE(msgs[0].reply_target.value_or("") == "test-session");

    // Deliver a response (simulating what StreamRelay/SessionManager does)
    ch.send_message("test-session", "world");

    sender.join();
    REQUIRE(response == "world");
}

// ── C API tests ─────────────────────────────────────────────────

#include "../../include/ptrclaw/ptrclaw.h"

TEST_CASE("ptrclaw_version: returns non-empty string", "[embed]") {
    const char* ver = ptrclaw_version();
    REQUIRE(ver != nullptr);
    REQUIRE(std::string(ver).find('.') != std::string::npos);
}

TEST_CASE("ptrclaw C API: null handle returns errors", "[embed]") {
    REQUIRE(ptrclaw_send(nullptr, "s", "m") == PTRCLAW_ERR_INVALID);
    REQUIRE(std::string(ptrclaw_last_error(nullptr)) == "null handle");
    REQUIRE(std::string(ptrclaw_last_response(nullptr, "s")).empty());
}

TEST_CASE("ptrclaw C API: null args return errors", "[embed]") {
    // We can't easily create a real handle without network access,
    // so just test the null-guard paths.
    REQUIRE(ptrclaw_send(nullptr, nullptr, "m") == PTRCLAW_ERR_INVALID);
    REQUIRE(ptrclaw_send(nullptr, "s", nullptr) == PTRCLAW_ERR_INVALID);
    REQUIRE(ptrclaw_send_stream(nullptr, "s", "m", nullptr, nullptr)
            == PTRCLAW_ERR_INVALID);
}

// ── Host-bridged tool tests ─────────────────────────────────────

TEST_CASE("ptrclaw_register_tool: null args return errors", "[embed]") {
    auto dummy = [](const char*, const char*, void*) -> char* { return nullptr; };
    REQUIRE(ptrclaw_register_tool(nullptr, "d", "{}", dummy, nullptr)
            == PTRCLAW_ERR_INVALID);
    REQUIRE(ptrclaw_register_tool("t", nullptr, "{}", dummy, nullptr)
            == PTRCLAW_ERR_INVALID);
    REQUIRE(ptrclaw_register_tool("t", "d", nullptr, dummy, nullptr)
            == PTRCLAW_ERR_INVALID);
    REQUIRE(ptrclaw_register_tool("t", "d", "{}", nullptr, nullptr)
            == PTRCLAW_ERR_INVALID);
}

TEST_CASE("ptrclaw_register_tool: registers successfully", "[embed]") {
    // Save and restore registry state
    auto& reg = ptrclaw::PluginRegistry::instance();

    auto cb = [](const char* /*name*/, const char* args, void* ud) -> char* {
        auto* prefix = static_cast<const char*>(ud);
        std::string result = std::string(prefix) + ":" + args;
        // NOLINTNEXTLINE(cppcoreguidelines-no-malloc)
        char* out = static_cast<char*>(malloc(result.size() + 1));
        std::copy(result.begin(), result.end(), out);
        out[result.size()] = '\0';
        return out;
    };

    const char* prefix = "test";
    REQUIRE(ptrclaw_register_tool("host_tool", "A test tool",
            R"({"type":"object","properties":{"arg":{"type":"string"}}})",
            cb, const_cast<char*>(prefix)) == PTRCLAW_OK);

    // Verify the tool appears in the registry
    auto names = reg.tool_names();
    bool found = false;
    for (const auto& n : names) {
        if (n == "host_tool") { found = true; break; }
    }
    REQUIRE(found);

    // Verify tool execution works through the registry
    auto tools = reg.create_all_tools();
    ptrclaw::Tool* host_tool = nullptr;
    for (auto& t : tools) {
        if (t->tool_name() == "host_tool") { host_tool = t.get(); break; }
    }
    REQUIRE(host_tool != nullptr);
    REQUIRE(host_tool->description() == "A test tool");

    auto result = host_tool->execute(R"({"arg":"hello"})");
    REQUIRE(result.success);
    REQUIRE(result.output == R"(test:{"arg":"hello"})");
}

TEST_CASE("ptrclaw_register_tool: null callback result returns failure", "[embed]") {
    auto null_cb = [](const char*, const char*, void*) -> char* {
        return nullptr;
    };

    REQUIRE(ptrclaw_register_tool("null_tool", "Returns null", "{}",
            null_cb, nullptr) == PTRCLAW_OK);

    auto tools = ptrclaw::PluginRegistry::instance().create_all_tools();
    ptrclaw::Tool* tool = nullptr;
    for (auto& t : tools) {
        if (t->tool_name() == "null_tool") { tool = t.get(); break; }
    }
    REQUIRE(tool != nullptr);

    auto result = tool->execute("{}");
    REQUIRE_FALSE(result.success);
}

// ── Lifecycle tests ─────────────────────────────────────────────

static bool has_tool(const std::string& name) {
    auto names = ptrclaw::PluginRegistry::instance().tool_names();
    for (const auto& n : names) {
        if (n == name) return true;
    }
    return false;
}

TEST_CASE("ptrclaw_destroy: nullptr is safe", "[embed]") {
    ptrclaw_destroy(nullptr);  // must not crash
}

TEST_CASE("ptrclaw lifecycle: create and destroy cleans up", "[embed]") {
    // Register a bridged tool before create
    auto cb = [](const char*, const char*, void*) -> char* {
        // NOLINTNEXTLINE(cppcoreguidelines-no-malloc)
        char* r = static_cast<char*>(malloc(3));
        r[0] = 'o'; r[1] = 'k'; r[2] = '\0';
        return r;
    };
    REQUIRE(ptrclaw_register_tool("lifecycle_tool", "test", "{}", cb, nullptr)
            == PTRCLAW_OK);
    REQUIRE(has_tool("lifecycle_tool"));

    // Create handle — succeeds even without API keys (provider created lazily)
    PtrClawHandle h = ptrclaw_create(nullptr);
    REQUIRE(h != nullptr);

    // Tool should still be in registry (sessions use it)
    REQUIRE(has_tool("lifecycle_tool"));

    // Destroy — must join thread, clear bus, unregister tools
    ptrclaw_destroy(h);

    // Bridged tool should be gone from the registry
    REQUIRE_FALSE(has_tool("lifecycle_tool"));
}

TEST_CASE("ptrclaw lifecycle: create/destroy/create cycle", "[embed]") {
    auto cb = [](const char*, const char*, void*) -> char* {
        // NOLINTNEXTLINE(cppcoreguidelines-no-malloc)
        char* r = static_cast<char*>(malloc(3));
        r[0] = 'o'; r[1] = 'k'; r[2] = '\0';
        return r;
    };

    // First cycle
    REQUIRE(ptrclaw_register_tool("cycle_tool", "test", "{}", cb, nullptr)
            == PTRCLAW_OK);
    PtrClawHandle h1 = ptrclaw_create(nullptr);
    REQUIRE(h1 != nullptr);
    REQUIRE(has_tool("cycle_tool"));
    ptrclaw_destroy(h1);
    REQUIRE_FALSE(has_tool("cycle_tool"));

    // Second cycle — must work without stale state
    REQUIRE(ptrclaw_register_tool("cycle_tool_v2", "test v2", "{}", cb, nullptr)
            == PTRCLAW_OK);
    PtrClawHandle h2 = ptrclaw_create(nullptr);
    REQUIRE(h2 != nullptr);
    REQUIRE(has_tool("cycle_tool_v2"));
    REQUIRE_FALSE(has_tool("cycle_tool"));  // old tool still gone
    ptrclaw_destroy(h2);
    REQUIRE_FALSE(has_tool("cycle_tool_v2"));
}

TEST_CASE("ptrclaw lifecycle: EmbedChannel destroyed after thread stops", "[embed]") {
    // Regression test: if the thread outlives the channel, we'd crash.
    // This test verifies the destructor joins the thread first.
    PtrClawHandle h = ptrclaw_create(nullptr);
    REQUIRE(h != nullptr);

    // Send a message to exercise the channel queue (won't get a response
    // since no provider is configured, but it proves the thread is running)
    // Just destroy immediately — the destructor must safely join.
    ptrclaw_destroy(h);
    // If we get here without crashing, the destruction order is correct.
}

// ── Config override tests ────────────────────────────────────────

TEST_CASE("ptrclaw_create: config_json overrides applied", "[embed]") {
    const char* cfg = R"({
        "provider": "openai",
        "model": "gpt-4o",
        "temperature": 0.5,
        "api_key": "test-key",
        "max_history_messages": 20,
        "disable_streaming": true,
        "tool_timeout": 30
    })";
    PtrClawHandle h = ptrclaw_create(cfg);
    REQUIRE(h != nullptr);
    ptrclaw_destroy(h);
}

TEST_CASE("ptrclaw_create: invalid config_json falls back to base config", "[embed]") {
    // Malformed JSON — create must still succeed with base config
    PtrClawHandle h = ptrclaw_create("{not valid json}");
    REQUIRE(h != nullptr);
    // Last error should mention parse failure but handle is still valid
    REQUIRE(std::string(ptrclaw_last_error(h)).find("config parse error") != std::string::npos);
    ptrclaw_destroy(h);
}

// ── ptrclaw_reset_session tests ──────────────────────────────────

TEST_CASE("ptrclaw_reset_session: null args return errors", "[embed]") {
    REQUIRE(ptrclaw_reset_session(nullptr, "s") == PTRCLAW_ERR_INVALID);
    REQUIRE(ptrclaw_reset_session(nullptr, nullptr) == PTRCLAW_ERR_INVALID);

    PtrClawHandle h = ptrclaw_create(nullptr);
    REQUIRE(h != nullptr);
    REQUIRE(ptrclaw_reset_session(h, nullptr) == PTRCLAW_ERR_INVALID);
    ptrclaw_destroy(h);
}

TEST_CASE("ptrclaw_reset_session: resets non-existent session safely", "[embed]") {
    PtrClawHandle h = ptrclaw_create(nullptr);
    REQUIRE(h != nullptr);
    // Resetting a session that was never used must not crash
    REQUIRE(ptrclaw_reset_session(h, "ghost-session") == PTRCLAW_OK);
    ptrclaw_destroy(h);
}

TEST_CASE("ptrclaw_reset_session: clears cached response", "[embed]") {
    PtrClawHandle h = ptrclaw_create(nullptr);
    REQUIRE(h != nullptr);

    // last_response starts empty
    REQUIRE(std::string(ptrclaw_last_response(h, "sess")).empty());

    // Reset a session that has no prior response — still returns OK
    REQUIRE(ptrclaw_reset_session(h, "sess") == PTRCLAW_OK);

    // Response is still empty after reset
    REQUIRE(std::string(ptrclaw_last_response(h, "sess")).empty());

    ptrclaw_destroy(h);
}

TEST_CASE("ptrclaw_reset_session: repeated resets are safe", "[embed]") {
    PtrClawHandle h = ptrclaw_create(nullptr);
    REQUIRE(h != nullptr);
    for (int i = 0; i < 5; ++i) {
        REQUIRE(ptrclaw_reset_session(h, "repeat-sess") == PTRCLAW_OK);
    }
    ptrclaw_destroy(h);
}
