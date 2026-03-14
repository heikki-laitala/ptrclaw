#include <catch2/catch_test_macros.hpp>
#include "../include/ptrclaw/ptrclaw.h"
#include <string>

// ── Helpers ──────────────────────────────────────────────────────

static const char *VALID_CFG =
    R"({"provider":"anthropic","api_key":"test-key","model":"claude-sonnet-4-6","tools":[]})";

// ── Version ──────────────────────────────────────────────────────

TEST_CASE("embed: ptrclaw_version returns non-empty string", "[embed]") {
    const char *v = ptrclaw_version();
    REQUIRE(v != nullptr);
    REQUIRE(std::string(v).size() > 0);
}

// ── Engine lifecycle ─────────────────────────────────────────────

TEST_CASE("embed: engine create / destroy", "[embed]") {
    SECTION("valid JSON creates engine") {
        PtrClawEngine eng = ptrclaw_engine_create(VALID_CFG);
        REQUIRE(eng != nullptr);
        REQUIRE(std::string(ptrclaw_engine_last_error(eng)).empty());
        ptrclaw_engine_destroy(eng);
    }

    SECTION("null config_json returns null") {
        PtrClawEngine eng = ptrclaw_engine_create(nullptr);
        REQUIRE(eng == nullptr);
    }

    SECTION("invalid JSON sets last_error but returns handle") {
        PtrClawEngine eng = ptrclaw_engine_create("{not valid json}");
        REQUIRE(eng != nullptr);
        std::string err = ptrclaw_engine_last_error(eng);
        REQUIRE(!err.empty());
        ptrclaw_engine_destroy(eng);
    }

    SECTION("last_error on null engine returns non-null message") {
        REQUIRE(ptrclaw_engine_last_error(nullptr) != nullptr);
    }
}

TEST_CASE("embed: engine config fields", "[embed]") {
    SECTION("empty JSON uses defaults (no crash)") {
        PtrClawEngine eng = ptrclaw_engine_create("{}");
        REQUIRE(eng != nullptr);
        ptrclaw_engine_destroy(eng);
    }

    SECTION("full config parses without error") {
        const char *cfg = R"({
            "provider": "anthropic",
            "api_key": "test-key",
            "model": "claude-opus-4-6",
            "temperature": 0.3,
            "tools": ["file_read"],
            "max_tool_iterations": 5,
            "max_history_messages": 20
        })";
        PtrClawEngine eng = ptrclaw_engine_create(cfg);
        REQUIRE(eng != nullptr);
        REQUIRE(std::string(ptrclaw_engine_last_error(eng)).empty());
        ptrclaw_engine_destroy(eng);
    }
}

// ── Session lifecycle ────────────────────────────────────────────

TEST_CASE("embed: session create / destroy", "[embed]") {
    PtrClawEngine eng = ptrclaw_engine_create(VALID_CFG);
    REQUIRE(eng != nullptr);

    SECTION("create and destroy with session_id") {
        PtrClawSession sess = ptrclaw_session_create(eng, "s1");
        REQUIRE(sess != nullptr);
        ptrclaw_session_destroy(sess);
    }

    SECTION("create with null session_id") {
        PtrClawSession sess = ptrclaw_session_create(eng, nullptr);
        REQUIRE(sess != nullptr);
        ptrclaw_session_destroy(sess);
    }

    SECTION("create with empty session_id") {
        PtrClawSession sess = ptrclaw_session_create(eng, "");
        REQUIRE(sess != nullptr);
        ptrclaw_session_destroy(sess);
    }

    SECTION("null engine returns null") {
        PtrClawSession sess = ptrclaw_session_create(nullptr, "s1");
        REQUIRE(sess == nullptr);
    }

    SECTION("multiple sessions from the same engine") {
        PtrClawSession s1 = ptrclaw_session_create(eng, "alice");
        PtrClawSession s2 = ptrclaw_session_create(eng, "bob");
        REQUIRE(s1 != nullptr);
        REQUIRE(s2 != nullptr);
        REQUIRE(s1 != s2);  // distinct handles
        ptrclaw_session_destroy(s1);
        ptrclaw_session_destroy(s2);
    }

    SECTION("clear_history on fresh session does not crash") {
        PtrClawSession sess = ptrclaw_session_create(eng, "s1");
        REQUIRE(sess != nullptr);
        ptrclaw_session_clear_history(sess);  // must not crash
        ptrclaw_session_destroy(sess);
    }

    SECTION("clear_history on null session does not crash") {
        ptrclaw_session_clear_history(nullptr);
    }

    SECTION("last_response on fresh session is empty string") {
        PtrClawSession sess = ptrclaw_session_create(eng, "s1");
        REQUIRE(sess != nullptr);
        const char *resp = ptrclaw_session_last_response(sess);
        REQUIRE(resp != nullptr);
        REQUIRE(std::string(resp).empty());
        ptrclaw_session_destroy(sess);
    }

    ptrclaw_engine_destroy(eng);
}

// ── Process error paths ──────────────────────────────────────────

TEST_CASE("embed: ptrclaw_process error paths", "[embed]") {
    PtrClawEngine eng = ptrclaw_engine_create(VALID_CFG);
    REQUIRE(eng != nullptr);
    PtrClawSession sess = ptrclaw_session_create(eng, "s1");
    REQUIRE(sess != nullptr);

    SECTION("null session returns PTRCLAW_ERR_INVALID") {
        REQUIRE(ptrclaw_process(nullptr, "hello") == PTRCLAW_ERR_INVALID);
    }

    SECTION("null message returns PTRCLAW_ERR_INVALID") {
        REQUIRE(ptrclaw_process(sess, nullptr) == PTRCLAW_ERR_INVALID);
    }

    ptrclaw_session_destroy(sess);
    ptrclaw_engine_destroy(eng);
}

TEST_CASE("embed: ptrclaw_process_stream error paths", "[embed]") {
    PtrClawEngine eng = ptrclaw_engine_create(VALID_CFG);
    REQUIRE(eng != nullptr);
    PtrClawSession sess = ptrclaw_session_create(eng, "s1");
    REQUIRE(sess != nullptr);

    SECTION("null session returns PTRCLAW_ERR_INVALID") {
        REQUIRE(ptrclaw_process_stream(nullptr, "hi",
            [](const char *, int, void *){}, nullptr) == PTRCLAW_ERR_INVALID);
    }

    SECTION("null message returns PTRCLAW_ERR_INVALID") {
        REQUIRE(ptrclaw_process_stream(sess, nullptr,
            [](const char *, int, void *){}, nullptr) == PTRCLAW_ERR_INVALID);
    }

    SECTION("null callback returns PTRCLAW_ERR_INVALID") {
        REQUIRE(ptrclaw_process_stream(sess, "hello", nullptr, nullptr)
            == PTRCLAW_ERR_INVALID);
    }

    ptrclaw_session_destroy(sess);
    ptrclaw_engine_destroy(eng);
}

// ── Tool filter ──────────────────────────────────────────────────

TEST_CASE("embed: tool filter in config", "[embed]") {
    SECTION("empty tools array creates session (no tools)") {
        PtrClawEngine eng = ptrclaw_engine_create(
            R"({"provider":"anthropic","api_key":"test","tools":[]})");
        REQUIRE(eng != nullptr);
        PtrClawSession sess = ptrclaw_session_create(eng, "");
        REQUIRE(sess != nullptr);
        ptrclaw_session_destroy(sess);
        ptrclaw_engine_destroy(eng);
    }

    SECTION("specific tool name in filter creates session") {
        PtrClawEngine eng = ptrclaw_engine_create(
            R"({"provider":"anthropic","api_key":"test","tools":["file_read"]})");
        REQUIRE(eng != nullptr);
        PtrClawSession sess = ptrclaw_session_create(eng, "");
        REQUIRE(sess != nullptr);
        ptrclaw_session_destroy(sess);
        ptrclaw_engine_destroy(eng);
    }

    SECTION("unrecognised tool name in filter does not crash") {
        PtrClawEngine eng = ptrclaw_engine_create(
            R"({"provider":"anthropic","api_key":"test","tools":["no_such_tool"]})");
        REQUIRE(eng != nullptr);
        PtrClawSession sess = ptrclaw_session_create(eng, "");
        REQUIRE(sess != nullptr);  // valid session, just zero tools
        ptrclaw_session_destroy(sess);
        ptrclaw_engine_destroy(eng);
    }
}
