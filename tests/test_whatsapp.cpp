#include <catch2/catch_test_macros.hpp>
#include "channels/whatsapp.hpp"
#include "mock_http_client.hpp"

using namespace ptrclaw;

static WhatsAppConfig make_config(std::vector<std::string> allow = {}) {
    WhatsAppConfig cfg;
    cfg.access_token = "test-token";
    cfg.phone_number_id = "123456";
    cfg.verify_token = "verify-secret";
    cfg.allow_from = std::move(allow);
    return cfg;
}

// ── channel_name ─────────────────────────────────────────────────

TEST_CASE("WhatsAppChannel: channel_name is whatsapp", "[whatsapp]") {
    MockHttpClient http;
    auto cfg = make_config();
    WhatsAppChannel ch(cfg, http);
    REQUIRE(ch.channel_name() == "whatsapp");
}

// ── health_check ─────────────────────────────────────────────────

TEST_CASE("WhatsAppChannel: health_check always returns true", "[whatsapp]") {
    MockHttpClient http;
    auto cfg = make_config();
    WhatsAppChannel ch(cfg, http);
    REQUIRE(ch.health_check());
}

// ── api_url ──────────────────────────────────────────────────────

TEST_CASE("WhatsAppChannel: api_url builds correct URL", "[whatsapp]") {
    MockHttpClient http;
    auto cfg = make_config();
    WhatsAppChannel ch(cfg, http);
    REQUIRE(ch.api_url() == "https://graph.facebook.com/v18.0/123456/messages");
}

// ── normalize_phone ──────────────────────────────────────────────

TEST_CASE("WhatsAppChannel: normalize_phone adds + prefix", "[whatsapp]") {
    REQUIRE(WhatsAppChannel::normalize_phone("1234567890") == "+1234567890");
}

TEST_CASE("WhatsAppChannel: normalize_phone keeps existing +", "[whatsapp]") {
    REQUIRE(WhatsAppChannel::normalize_phone("+1234567890") == "+1234567890");
}

TEST_CASE("WhatsAppChannel: normalize_phone empty string", "[whatsapp]") {
    REQUIRE(WhatsAppChannel::normalize_phone("").empty());
}

// ── is_number_allowed ────────────────────────────────────────────

TEST_CASE("WhatsAppChannel: is_number_allowed empty allowlist allows all", "[whatsapp]") {
    REQUIRE(WhatsAppChannel::is_number_allowed("+1234567890", {}));
}

TEST_CASE("WhatsAppChannel: is_number_allowed wildcard allows all", "[whatsapp]") {
    REQUIRE(WhatsAppChannel::is_number_allowed("+1234567890", {"*"}));
}

TEST_CASE("WhatsAppChannel: is_number_allowed exact match", "[whatsapp]") {
    REQUIRE(WhatsAppChannel::is_number_allowed("+1234567890", {"+1234567890"}));
}

TEST_CASE("WhatsAppChannel: is_number_allowed normalizes for comparison", "[whatsapp]") {
    REQUIRE(WhatsAppChannel::is_number_allowed("1234567890", {"+1234567890"}));
    REQUIRE(WhatsAppChannel::is_number_allowed("+1234567890", {"1234567890"}));
}

TEST_CASE("WhatsAppChannel: is_number_allowed rejects unlisted", "[whatsapp]") {
    REQUIRE_FALSE(WhatsAppChannel::is_number_allowed("+9999999999", {"+1234567890"}));
}

// ── verify_token ─────────────────────────────────────────────────

TEST_CASE("WhatsAppChannel: verify_token returns configured token", "[whatsapp]") {
    MockHttpClient http;
    auto cfg = make_config();
    WhatsAppChannel ch(cfg, http);
    REQUIRE(ch.verify_token() == "verify-secret");
}

// ── send_message ─────────────────────────────────────────────────

TEST_CASE("WhatsAppChannel: send_message posts to correct URL", "[whatsapp]") {
    MockHttpClient http;
    http.next_response = {200, R"({"messages":[{"id":"wamid.123"}]})"};
    auto cfg = make_config();
    WhatsAppChannel ch(cfg, http);

    ch.send_message("+1234567890", "Hello!");

    REQUIRE(http.last_url == "https://graph.facebook.com/v18.0/123456/messages");
    REQUIRE(http.call_count == 1);
    // Check authorization header
    bool has_auth = false;
    for (const auto& h : http.last_headers) {
        if (h.first == "Authorization" && h.second == "Bearer test-token") {
            has_auth = true;
        }
    }
    REQUIRE(has_auth);
}

TEST_CASE("WhatsAppChannel: send_message strips + from target", "[whatsapp]") {
    MockHttpClient http;
    http.next_response = {200, "{}"};
    auto cfg = make_config();
    WhatsAppChannel ch(cfg, http);

    ch.send_message("+1234567890", "hi");
    // The body should contain "to":"1234567890" (without +)
    REQUIRE(http.last_body.find("1234567890") != std::string::npos);
    // Should NOT have the + in the to field
    REQUIRE(http.last_body.find("\"+1234567890\"") == std::string::npos);
}

// ── parse_webhook_payload ────────────────────────────────────────

TEST_CASE("WhatsAppChannel: parse valid text message", "[whatsapp]") {
    MockHttpClient http;
    auto cfg = make_config({"*"});
    WhatsAppChannel ch(cfg, http);

    std::string payload = R"({
        "entry": [{
            "changes": [{
                "value": {
                    "messages": [{
                        "from": "1234567890",
                        "type": "text",
                        "text": {"body": "Hello!"},
                        "timestamp": "1700000000"
                    }]
                }
            }]
        }]
    })";

    auto msgs = ch.parse_webhook_payload(payload);
    REQUIRE(msgs.size() == 1);
    REQUIRE(msgs[0].sender == "+1234567890");
    REQUIRE(msgs[0].content == "Hello!");
    REQUIRE(msgs[0].timestamp == 1700000000);
}

TEST_CASE("WhatsAppChannel: parse filters unauthorized sender", "[whatsapp]") {
    MockHttpClient http;
    auto cfg = make_config({"+9999999999"});
    WhatsAppChannel ch(cfg, http);

    std::string payload = R"({
        "entry": [{
            "changes": [{
                "value": {
                    "messages": [{
                        "from": "1234567890",
                        "type": "text",
                        "text": {"body": "sneaky"},
                        "timestamp": "0"
                    }]
                }
            }]
        }]
    })";

    auto msgs = ch.parse_webhook_payload(payload);
    REQUIRE(msgs.empty());
}

TEST_CASE("WhatsAppChannel: parse filters non-text messages", "[whatsapp]") {
    MockHttpClient http;
    auto cfg = make_config({"*"});
    WhatsAppChannel ch(cfg, http);

    std::string payload = R"({
        "entry": [{
            "changes": [{
                "value": {
                    "messages": [
                        {"from": "123", "type": "image", "image": {}},
                        {"from": "123", "type": "audio", "audio": {}},
                        {"from": "123", "type": "video", "video": {}},
                        {"from": "123", "type": "document", "document": {}},
                        {"from": "123", "type": "sticker", "sticker": {}},
                        {"from": "123", "type": "location", "location": {}}
                    ]
                }
            }]
        }]
    })";

    auto msgs = ch.parse_webhook_payload(payload);
    REQUIRE(msgs.empty());
}

TEST_CASE("WhatsAppChannel: parse handles invalid JSON", "[whatsapp]") {
    MockHttpClient http;
    auto cfg = make_config({"*"});
    WhatsAppChannel ch(cfg, http);

    auto msgs = ch.parse_webhook_payload("not json {{{");
    REQUIRE(msgs.empty());
}

TEST_CASE("WhatsAppChannel: parse handles missing fields gracefully", "[whatsapp]") {
    MockHttpClient http;
    auto cfg = make_config({"*"});
    WhatsAppChannel ch(cfg, http);

    // Missing entry
    auto msgs1 = ch.parse_webhook_payload(R"({})");
    REQUIRE(msgs1.empty());

    // Missing changes
    auto msgs2 = ch.parse_webhook_payload(R"({"entry": [{}]})");
    REQUIRE(msgs2.empty());

    // Missing value
    auto msgs3 = ch.parse_webhook_payload(R"({"entry": [{"changes": [{}]}]})");
    REQUIRE(msgs3.empty());

    // Missing messages
    auto msgs4 = ch.parse_webhook_payload(R"({"entry": [{"changes": [{"value": {}}]}]})");
    REQUIRE(msgs4.empty());
}

TEST_CASE("WhatsAppChannel: parse handles multiple messages", "[whatsapp]") {
    MockHttpClient http;
    auto cfg = make_config({"*"});
    WhatsAppChannel ch(cfg, http);

    std::string payload = R"({
        "entry": [{
            "changes": [{
                "value": {
                    "messages": [
                        {"from": "111", "type": "text", "text": {"body": "first"}, "timestamp": "1"},
                        {"from": "222", "type": "text", "text": {"body": "second"}, "timestamp": "2"}
                    ]
                }
            }]
        }]
    })";

    auto msgs = ch.parse_webhook_payload(payload);
    REQUIRE(msgs.size() == 2);
    REQUIRE(msgs[0].content == "first");
    REQUIRE(msgs[1].content == "second");
}

TEST_CASE("WhatsAppChannel: parse handles missing sender", "[whatsapp]") {
    MockHttpClient http;
    auto cfg = make_config({"*"});
    WhatsAppChannel ch(cfg, http);

    std::string payload = R"({
        "entry": [{
            "changes": [{
                "value": {
                    "messages": [{
                        "type": "text",
                        "text": {"body": "no sender"},
                        "timestamp": "0"
                    }]
                }
            }]
        }]
    })";

    auto msgs = ch.parse_webhook_payload(payload);
    REQUIRE(msgs.empty());
}

TEST_CASE("WhatsAppChannel: parse handles invalid timestamp", "[whatsapp]") {
    MockHttpClient http;
    auto cfg = make_config({"*"});
    WhatsAppChannel ch(cfg, http);

    std::string payload = R"({
        "entry": [{
            "changes": [{
                "value": {
                    "messages": [{
                        "from": "123",
                        "type": "text",
                        "text": {"body": "hi"},
                        "timestamp": "not-a-number"
                    }]
                }
            }]
        }]
    })";

    auto msgs = ch.parse_webhook_payload(payload);
    REQUIRE(msgs.size() == 1);
    // Should fall back to current epoch, which will be > 0
    REQUIRE(msgs[0].timestamp > 0);
}

TEST_CASE("WhatsAppChannel: parse mixed authorized and unauthorized", "[whatsapp]") {
    MockHttpClient http;
    auto cfg = make_config({"+111"});
    WhatsAppChannel ch(cfg, http);

    std::string payload = R"({
        "entry": [{
            "changes": [{
                "value": {
                    "messages": [
                        {"from": "111", "type": "text", "text": {"body": "allowed"}, "timestamp": "0"},
                        {"from": "222", "type": "text", "text": {"body": "denied"}, "timestamp": "0"}
                    ]
                }
            }]
        }]
    })";

    auto msgs = ch.parse_webhook_payload(payload);
    REQUIRE(msgs.size() == 1);
    REQUIRE(msgs[0].content == "allowed");
}

TEST_CASE("WhatsAppChannel: parse unicode message", "[whatsapp]") {
    MockHttpClient http;
    auto cfg = make_config({"*"});
    WhatsAppChannel ch(cfg, http);

    std::string payload = R"({
        "entry": [{
            "changes": [{
                "value": {
                    "messages": [{
                        "from": "123",
                        "type": "text",
                        "text": {"body": "Hello \u4e16\u754c \ud83d\ude00"},
                        "timestamp": "0"
                    }]
                }
            }]
        }]
    })";

    auto msgs = ch.parse_webhook_payload(payload);
    REQUIRE(msgs.size() == 1);
    REQUIRE_FALSE(msgs[0].content.empty());
}
