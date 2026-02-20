#include <catch2/catch_test_macros.hpp>
#include "channels/telegram.hpp"
#include "mock_http_client.hpp"

using namespace ptrclaw;

static TelegramConfig make_config(const std::string& token = "test-token",
                                   std::vector<std::string> allow = {}) {
    TelegramConfig cfg;
    cfg.bot_token = token;
    cfg.allow_from = std::move(allow);
    return cfg;
}

// ── api_url ──────────────────────────────────────────────────────

TEST_CASE("TelegramChannel: api_url builds correct URL", "[telegram]") {
    MockHttpClient http;
    auto cfg = make_config("123:ABC");
    TelegramChannel ch(cfg, http);
    REQUIRE(ch.api_url("getMe") == "https://api.telegram.org/bot123:ABC/getMe");
    REQUIRE(ch.api_url("sendMessage") == "https://api.telegram.org/bot123:ABC/sendMessage");
    REQUIRE(ch.api_url("getUpdates") == "https://api.telegram.org/bot123:ABC/getUpdates");
}

// ── channel_name ─────────────────────────────────────────────────

TEST_CASE("TelegramChannel: channel_name is telegram", "[telegram]") {
    MockHttpClient http;
    auto cfg = make_config();
    TelegramChannel ch(cfg, http);
    REQUIRE(ch.channel_name() == "telegram");
}

// ── is_user_allowed ──────────────────────────────────────────────

TEST_CASE("TelegramChannel: is_user_allowed with empty allowlist allows all", "[telegram]") {
    REQUIRE(TelegramChannel::is_user_allowed("anyone", {}));
}

TEST_CASE("TelegramChannel: is_user_allowed wildcard allows all", "[telegram]") {
    REQUIRE(TelegramChannel::is_user_allowed("anyone", {"*"}));
}

TEST_CASE("TelegramChannel: is_user_allowed case insensitive", "[telegram]") {
    REQUIRE(TelegramChannel::is_user_allowed("Alice", {"alice"}));
    REQUIRE(TelegramChannel::is_user_allowed("alice", {"ALICE"}));
    REQUIRE(TelegramChannel::is_user_allowed("AlIcE", {"aLiCe"}));
}

TEST_CASE("TelegramChannel: is_user_allowed strips @ from allowlist", "[telegram]") {
    REQUIRE(TelegramChannel::is_user_allowed("alice", {"@alice"}));
    REQUIRE(TelegramChannel::is_user_allowed("bob", {"@Bob"}));
}

TEST_CASE("TelegramChannel: is_user_allowed rejects unlisted user", "[telegram]") {
    REQUIRE_FALSE(TelegramChannel::is_user_allowed("eve", {"alice", "bob"}));
}

TEST_CASE("TelegramChannel: is_user_allowed numeric user ID", "[telegram]") {
    REQUIRE(TelegramChannel::is_user_allowed("12345", {"12345"}));
    REQUIRE_FALSE(TelegramChannel::is_user_allowed("12345", {"67890"}));
}

// ── health_check ─────────────────────────────────────────────────

TEST_CASE("TelegramChannel: health_check returns true on valid response", "[telegram]") {
    MockHttpClient http;
    http.next_response = {200, R"({"ok":true,"result":{"id":123,"first_name":"Bot"}})"};
    auto cfg = make_config();
    TelegramChannel ch(cfg, http);
    REQUIRE(ch.health_check());
}

TEST_CASE("TelegramChannel: health_check returns false on error", "[telegram]") {
    MockHttpClient http;
    http.next_response = {401, R"({"ok":false})"};
    auto cfg = make_config();
    TelegramChannel ch(cfg, http);
    REQUIRE_FALSE(ch.health_check());
}

TEST_CASE("TelegramChannel: health_check returns false on invalid JSON", "[telegram]") {
    MockHttpClient http;
    http.next_response = {200, "not json"};
    auto cfg = make_config();
    TelegramChannel ch(cfg, http);
    REQUIRE_FALSE(ch.health_check());
}

// ── poll_updates ─────────────────────────────────────────────────

TEST_CASE("TelegramChannel: poll_updates parses text message", "[telegram]") {
    MockHttpClient http;
    http.next_response = {200, R"({
        "ok": true,
        "result": [{
            "update_id": 100,
            "message": {
                "message_id": 1,
                "from": {"id": 42, "username": "alice", "first_name": "Alice"},
                "chat": {"id": -100, "type": "private"},
                "date": 1700000000,
                "text": "Hello bot"
            }
        }]
    })"};

    auto cfg = make_config("tok", {"alice"});
    TelegramChannel ch(cfg, http);
    auto msgs = ch.poll_updates();

    REQUIRE(msgs.size() == 1);
    REQUIRE(msgs[0].sender == "alice");
    REQUIRE(msgs[0].content == "Hello bot");
    REQUIRE(msgs[0].channel == "telegram");
    REQUIRE(msgs[0].timestamp == 1700000000);
    REQUIRE(msgs[0].reply_target == "-100");
    REQUIRE(msgs[0].message_id == 1);
    REQUIRE(msgs[0].first_name == "Alice");
    REQUIRE_FALSE(msgs[0].is_group);
}

TEST_CASE("TelegramChannel: poll_updates advances update_id", "[telegram]") {
    MockHttpClient http;
    http.next_response = {200, R"({
        "ok": true,
        "result": [{
            "update_id": 50,
            "message": {
                "message_id": 1,
                "from": {"id": 1, "username": "u"},
                "chat": {"id": 1, "type": "private"},
                "date": 0,
                "text": "hi"
            }
        }]
    })"};

    auto cfg = make_config("tok", {"*"});
    TelegramChannel ch(cfg, http);
    ch.poll_updates();
    REQUIRE(ch.last_update_id() == 51);
}

TEST_CASE("TelegramChannel: poll_updates filters unauthorized users", "[telegram]") {
    MockHttpClient http;
    http.next_response = {200, R"({
        "ok": true,
        "result": [{
            "update_id": 1,
            "message": {
                "message_id": 1,
                "from": {"id": 99, "username": "eve"},
                "chat": {"id": 1, "type": "private"},
                "date": 0,
                "text": "sneaky"
            }
        }]
    })"};

    auto cfg = make_config("tok", {"alice"});
    TelegramChannel ch(cfg, http);
    auto msgs = ch.poll_updates();
    REQUIRE(msgs.empty());
}

TEST_CASE("TelegramChannel: poll_updates skips empty text", "[telegram]") {
    MockHttpClient http;
    http.next_response = {200, R"({
        "ok": true,
        "result": [{
            "update_id": 1,
            "message": {
                "message_id": 1,
                "from": {"id": 1, "username": "u"},
                "chat": {"id": 1, "type": "private"},
                "date": 0
            }
        }]
    })"};

    auto cfg = make_config("tok", {"*"});
    TelegramChannel ch(cfg, http);
    auto msgs = ch.poll_updates();
    REQUIRE(msgs.empty());
}

TEST_CASE("TelegramChannel: poll_updates detects group chat", "[telegram]") {
    MockHttpClient http;
    http.next_response = {200, R"({
        "ok": true,
        "result": [{
            "update_id": 1,
            "message": {
                "message_id": 1,
                "from": {"id": 1, "username": "u"},
                "chat": {"id": -200, "type": "supergroup"},
                "date": 0,
                "text": "hi"
            }
        }]
    })"};

    auto cfg = make_config("tok", {"*"});
    TelegramChannel ch(cfg, http);
    auto msgs = ch.poll_updates();
    REQUIRE(msgs.size() == 1);
    REQUIRE(msgs[0].is_group);
}

TEST_CASE("TelegramChannel: poll_updates handles HTTP error", "[telegram]") {
    MockHttpClient http;
    http.next_response = {500, ""};
    auto cfg = make_config("tok", {"*"});
    TelegramChannel ch(cfg, http);
    auto msgs = ch.poll_updates();
    REQUIRE(msgs.empty());
}

TEST_CASE("TelegramChannel: poll_updates handles invalid JSON", "[telegram]") {
    MockHttpClient http;
    http.next_response = {200, "not json"};
    auto cfg = make_config("tok", {"*"});
    TelegramChannel ch(cfg, http);
    auto msgs = ch.poll_updates();
    REQUIRE(msgs.empty());
}

TEST_CASE("TelegramChannel: poll_updates handles multiple messages", "[telegram]") {
    MockHttpClient http;
    http.next_response = {200, R"({
        "ok": true,
        "result": [
            {
                "update_id": 1,
                "message": {
                    "message_id": 1,
                    "from": {"id": 1, "username": "alice"},
                    "chat": {"id": 1, "type": "private"},
                    "date": 0,
                    "text": "msg1"
                }
            },
            {
                "update_id": 2,
                "message": {
                    "message_id": 2,
                    "from": {"id": 2, "username": "bob"},
                    "chat": {"id": 2, "type": "private"},
                    "date": 0,
                    "text": "msg2"
                }
            }
        ]
    })"};

    auto cfg = make_config("tok", {"*"});
    TelegramChannel ch(cfg, http);
    auto msgs = ch.poll_updates();
    REQUIRE(msgs.size() == 2);
    REQUIRE(msgs[0].content == "msg1");
    REQUIRE(msgs[1].content == "msg2");
}

// ── send_message ─────────────────────────────────────────────────

TEST_CASE("TelegramChannel: send_message posts to correct URL", "[telegram]") {
    MockHttpClient http;
    http.next_response = {200, R"({"ok":true})"};
    auto cfg = make_config("tok123");
    TelegramChannel ch(cfg, http);

    ch.send_message("12345", "Hello!");
    REQUIRE(http.last_url == "https://api.telegram.org/bottok123/sendMessage");
    REQUIRE(http.call_count == 1);
}

TEST_CASE("TelegramChannel: send_message falls back to plain text on HTML failure", "[telegram]") {
    MockHttpClient http;
    // First call (HTML) fails, second (plain) succeeds
    http.next_response = {400, R"({"ok":false})"};
    auto cfg = make_config("tok");
    TelegramChannel ch(cfg, http);

    ch.send_message("1", "Hello");
    REQUIRE(http.call_count == 2);
}

// ── markdown_to_telegram_html ────────────────────────────────────

TEST_CASE("TelegramChannel: markdown bold conversion", "[telegram]") {
    auto html = TelegramChannel::markdown_to_telegram_html("This is **bold** text");
    REQUIRE(html.find("<b>bold</b>") != std::string::npos);
}

TEST_CASE("TelegramChannel: markdown italic conversion", "[telegram]") {
    auto html = TelegramChannel::markdown_to_telegram_html("This is _italic_ text");
    REQUIRE(html.find("<i>italic</i>") != std::string::npos);
}

TEST_CASE("TelegramChannel: markdown code block conversion", "[telegram]") {
    auto html = TelegramChannel::markdown_to_telegram_html("```\ncode here\n```");
    REQUIRE(html.find("<pre>code here</pre>") != std::string::npos);
}

TEST_CASE("TelegramChannel: markdown inline code conversion", "[telegram]") {
    auto html = TelegramChannel::markdown_to_telegram_html("Use `code` here");
    REQUIRE(html.find("<code>code</code>") != std::string::npos);
}

TEST_CASE("TelegramChannel: markdown strikethrough conversion", "[telegram]") {
    auto html = TelegramChannel::markdown_to_telegram_html("This is ~~deleted~~ text");
    REQUIRE(html.find("<s>deleted</s>") != std::string::npos);
}

TEST_CASE("TelegramChannel: markdown link conversion", "[telegram]") {
    auto html = TelegramChannel::markdown_to_telegram_html("Click [here](https://example.com)");
    REQUIRE(html.find("<a href=\"https://example.com\">here</a>") != std::string::npos);
}

TEST_CASE("TelegramChannel: markdown header conversion", "[telegram]") {
    auto html = TelegramChannel::markdown_to_telegram_html("# Title");
    REQUIRE(html.find("<b>Title</b>") != std::string::npos);
}

TEST_CASE("TelegramChannel: markdown bullet list conversion", "[telegram]") {
    auto html = TelegramChannel::markdown_to_telegram_html("- Item one");
    REQUIRE(html.find("\u2022") != std::string::npos); // •
}

TEST_CASE("TelegramChannel: HTML escaping in plain text", "[telegram]") {
    auto html = TelegramChannel::markdown_to_telegram_html("1 < 2 & 3 > 0");
    REQUIRE(html.find("&lt;") != std::string::npos);
    REQUIRE(html.find("&amp;") != std::string::npos);
    REQUIRE(html.find("&gt;") != std::string::npos);
}

TEST_CASE("TelegramChannel: plain text passthrough", "[telegram]") {
    auto html = TelegramChannel::markdown_to_telegram_html("Just plain text");
    REQUIRE(html == "Just plain text");
}

// ── set_my_commands ──────────────────────────────────────────────

TEST_CASE("TelegramChannel: set_my_commands sends request", "[telegram]") {
    MockHttpClient http;
    http.next_response = {200, R"({"ok":true})"};
    auto cfg = make_config("tok");
    TelegramChannel ch(cfg, http);

    REQUIRE(ch.set_my_commands());
    REQUIRE(http.last_url.find("setMyCommands") != std::string::npos);
}

// ── drop_pending_updates ─────────────────────────────────────────

TEST_CASE("TelegramChannel: drop_pending_updates advances offset", "[telegram]") {
    MockHttpClient http;
    http.next_response = {200, R"({
        "ok": true,
        "result": [{"update_id": 99}]
    })"};
    auto cfg = make_config("tok");
    TelegramChannel ch(cfg, http);

    REQUIRE(ch.drop_pending_updates());
    REQUIRE(ch.last_update_id() == 100);
}

TEST_CASE("TelegramChannel: drop_pending_updates with empty result", "[telegram]") {
    MockHttpClient http;
    http.next_response = {200, R"({"ok": true, "result": []})"};
    auto cfg = make_config("tok");
    TelegramChannel ch(cfg, http);

    REQUIRE(ch.drop_pending_updates());
    REQUIRE(ch.last_update_id() == 0);
}
