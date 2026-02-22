#include <catch2/catch_test_macros.hpp>
#include "channels/whatsapp.hpp"
#include "channels/webhook_server.hpp"
#include "mock_http_client.hpp"

using namespace ptrclaw;

static WhatsAppConfig make_webhook_config() {
    WhatsAppConfig cfg;
    cfg.access_token    = "test-token";
    cfg.phone_number_id = "123456";
    cfg.verify_token    = "verify-secret";
    cfg.allow_from      = {"*"};
    cfg.webhook_listen  = "127.0.0.1:8080";
    cfg.webhook_max_body = 65536;
    return cfg;
}

// ── parse_listen_addr ─────────────────────────────────────────────────────────

TEST_CASE("parse_listen_addr: valid host:port", "[whatsapp_webhook]") {
    std::string host; uint16_t port;
    REQUIRE(parse_listen_addr("127.0.0.1:8080", host, port));
    REQUIRE(host == "127.0.0.1");
    REQUIRE(port == 8080);
}

TEST_CASE("parse_listen_addr: missing colon returns false", "[whatsapp_webhook]") {
    std::string host; uint16_t port;
    REQUIRE_FALSE(parse_listen_addr("127.0.0.1", host, port));
}

TEST_CASE("parse_listen_addr: non-numeric port returns false", "[whatsapp_webhook]") {
    std::string host; uint16_t port;
    REQUIRE_FALSE(parse_listen_addr("127.0.0.1:notaport", host, port));
}

TEST_CASE("parse_listen_addr: empty string returns false", "[whatsapp_webhook]") {
    std::string host; uint16_t port;
    REQUIRE_FALSE(parse_listen_addr("", host, port));
}

TEST_CASE("parse_listen_addr: port 0 is rejected", "[whatsapp_webhook]") {
    std::string host; uint16_t port;
    REQUIRE_FALSE(parse_listen_addr("127.0.0.1:0", host, port));
}

// ── WebhookRequest::query_param ───────────────────────────────────────────────

TEST_CASE("WebhookRequest::query_param: basic lookup", "[whatsapp_webhook]") {
    WebhookRequest req;
    req.query_params = {{"hub.mode", "subscribe"}, {"hub.verify_token", "secret"}, {"hub.challenge", "abc123"}};
    REQUIRE(req.query_param("hub.mode") == "subscribe");
    REQUIRE(req.query_param("hub.verify_token") == "secret");
    REQUIRE(req.query_param("hub.challenge") == "abc123");
}

TEST_CASE("WebhookRequest::query_param: missing key returns empty", "[whatsapp_webhook]") {
    WebhookRequest req;
    req.query_params = {{"key", "val"}};
    REQUIRE(req.query_param("other").empty());
}

TEST_CASE("WebhookRequest::query_param: empty query params", "[whatsapp_webhook]") {
    WebhookRequest req;
    REQUIRE(req.query_param("anything").empty());
}

// ── GET verify handshake ──────────────────────────────────────────────────────

TEST_CASE("WhatsApp webhook: GET verify returns challenge on match", "[whatsapp_webhook]") {
    MockHttpClient http;
    WhatsAppChannel ch(make_webhook_config(), http);

    WebhookRequest req;
    req.method = "GET";
    req.path   = "/webhook";
    req.query_params = {{"hub.mode", "subscribe"}, {"hub.verify_token", "verify-secret"}, {"hub.challenge", "abc123"}};

    auto resp = ch.handle_webhook_request(req);
    REQUIRE(resp.status == 200);
    REQUIRE(resp.body == "abc123");
}

TEST_CASE("WhatsApp webhook: GET verify wrong token returns 403", "[whatsapp_webhook]") {
    MockHttpClient http;
    WhatsAppChannel ch(make_webhook_config(), http);

    WebhookRequest req;
    req.method = "GET";
    req.path   = "/webhook";
    req.query_params = {{"hub.mode", "subscribe"}, {"hub.verify_token", "wrong"}, {"hub.challenge", "abc123"}};

    auto resp = ch.handle_webhook_request(req);
    REQUIRE(resp.status == 403);
}

TEST_CASE("WhatsApp webhook: GET verify missing mode returns 403", "[whatsapp_webhook]") {
    MockHttpClient http;
    WhatsAppChannel ch(make_webhook_config(), http);

    WebhookRequest req;
    req.method = "GET";
    req.path   = "/webhook";
    req.query_params = {{"hub.verify_token", "verify-secret"}, {"hub.challenge", "abc123"}}; // no hub.mode

    auto resp = ch.handle_webhook_request(req);
    REQUIRE(resp.status == 403);
}

TEST_CASE("WhatsApp webhook: GET verify wrong mode returns 403", "[whatsapp_webhook]") {
    MockHttpClient http;
    WhatsAppChannel ch(make_webhook_config(), http);

    WebhookRequest req;
    req.method = "GET";
    req.path   = "/webhook";
    req.query_params = {{"hub.mode", "unsubscribe"}, {"hub.verify_token", "verify-secret"}, {"hub.challenge", "x"}};

    auto resp = ch.handle_webhook_request(req);
    REQUIRE(resp.status == 403);
}

TEST_CASE("WhatsApp webhook: GET verify with empty verify_token returns 403", "[whatsapp_webhook]") {
    MockHttpClient http;
    auto cfg = make_webhook_config();
    cfg.verify_token = "";
    WhatsAppChannel ch(cfg, http);

    WebhookRequest req;
    req.method = "GET";
    req.path   = "/webhook";
    req.query_params = {{"hub.mode", "subscribe"}, {"hub.verify_token", ""}, {"hub.challenge", "x"}};

    auto resp = ch.handle_webhook_request(req);
    REQUIRE(resp.status == 403);
}

// ── POST without shared secret configured ────────────────────────────────────

TEST_CASE("WhatsApp webhook: POST no secret configured returns 200", "[whatsapp_webhook]") {
    MockHttpClient http;
    auto cfg = make_webhook_config();
    cfg.webhook_secret = "";
    WhatsAppChannel ch(cfg, http);

    WebhookRequest req;
    req.method = "POST";
    req.path   = "/webhook";
    req.body   = R"({"entry":[]})";

    auto resp = ch.handle_webhook_request(req);
    REQUIRE(resp.status == 200);
}

// ── POST with shared secret ───────────────────────────────────────────────────

TEST_CASE("WhatsApp webhook: POST correct secret returns 200", "[whatsapp_webhook]") {
    MockHttpClient http;
    auto cfg = make_webhook_config();
    cfg.webhook_secret = "proxy-secret";
    WhatsAppChannel ch(cfg, http);

    WebhookRequest req;
    req.method = "POST";
    req.path   = "/webhook";
    req.headers["x-webhook-secret"] = "proxy-secret";
    req.body   = R"({"entry":[]})";

    auto resp = ch.handle_webhook_request(req);
    REQUIRE(resp.status == 200);
}

TEST_CASE("WhatsApp webhook: POST wrong secret returns 403", "[whatsapp_webhook]") {
    MockHttpClient http;
    auto cfg = make_webhook_config();
    cfg.webhook_secret = "proxy-secret";
    WhatsAppChannel ch(cfg, http);

    WebhookRequest req;
    req.method = "POST";
    req.path   = "/webhook";
    req.headers["x-webhook-secret"] = "wrong";
    req.body   = R"({"entry":[]})";

    auto resp = ch.handle_webhook_request(req);
    REQUIRE(resp.status == 403);
}

TEST_CASE("WhatsApp webhook: POST missing secret header returns 403", "[whatsapp_webhook]") {
    MockHttpClient http;
    auto cfg = make_webhook_config();
    cfg.webhook_secret = "proxy-secret";
    WhatsAppChannel ch(cfg, http);

    WebhookRequest req;
    req.method = "POST";
    req.path   = "/webhook";
    // No x-webhook-secret header
    req.body   = R"({"entry":[]})";

    auto resp = ch.handle_webhook_request(req);
    REQUIRE(resp.status == 403);
}

// ── POST payload ingestion ────────────────────────────────────────────────────

static const char* kValidPayload = R"({
    "entry": [{
        "changes": [{
            "value": {
                "messages": [{
                    "from": "1234567890",
                    "type": "text",
                    "text": {"body": "Hello webhook!"},
                    "timestamp": "1700000000"
                }]
            }
        }]
    }]
})";

TEST_CASE("WhatsApp webhook: POST valid payload queues message", "[whatsapp_webhook]") {
    MockHttpClient http;
    auto cfg = make_webhook_config();
    cfg.webhook_secret = "";
    WhatsAppChannel ch(cfg, http);

    WebhookRequest req;
    req.method = "POST";
    req.path   = "/webhook";
    req.body   = kValidPayload;

    auto resp = ch.handle_webhook_request(req);
    REQUIRE(resp.status == 200);

    // poll_updates drains the queue immediately (messages already present)
    auto msgs = ch.poll_updates();
    REQUIRE(msgs.size() == 1);
    REQUIRE(msgs[0].sender      == "+1234567890");
    REQUIRE(msgs[0].content     == "Hello webhook!");
    REQUIRE(msgs[0].channel     == "whatsapp");
    REQUIRE(msgs[0].timestamp   == 1700000000);
    REQUIRE(msgs[0].reply_target.value_or("") == "+1234567890");
}

TEST_CASE("WhatsApp webhook: POST empty entry array returns 200 no messages", "[whatsapp_webhook]") {
    MockHttpClient http;
    auto cfg = make_webhook_config();
    cfg.webhook_secret = "";
    WhatsAppChannel ch(cfg, http);

    WebhookRequest req;
    req.method = "POST";
    req.path   = "/webhook";
    req.body   = R"({"entry":[]})";

    auto resp = ch.handle_webhook_request(req);
    REQUIRE(resp.status == 200);

    auto msgs = ch.poll_updates();
    REQUIRE(msgs.empty());
}

TEST_CASE("WhatsApp webhook: POST unauthorized sender not queued", "[whatsapp_webhook]") {
    MockHttpClient http;
    auto cfg = make_webhook_config();
    cfg.allow_from    = {"+9999999999"};  // only this number allowed
    cfg.webhook_secret = "";
    WhatsAppChannel ch(cfg, http);

    WebhookRequest req;
    req.method = "POST";
    req.path   = "/webhook";
    req.body   = kValidPayload;  // sender is +1234567890

    ch.handle_webhook_request(req);
    auto msgs = ch.poll_updates();
    REQUIRE(msgs.empty());
}

// ── Unsupported methods ───────────────────────────────────────────────────────

TEST_CASE("WhatsApp webhook: DELETE returns 405", "[whatsapp_webhook]") {
    MockHttpClient http;
    WhatsAppChannel ch(make_webhook_config(), http);

    WebhookRequest req;
    req.method = "DELETE";
    req.path   = "/webhook";

    auto resp = ch.handle_webhook_request(req);
    REQUIRE(resp.status == 405);
}

TEST_CASE("WhatsApp webhook: PUT returns 405", "[whatsapp_webhook]") {
    MockHttpClient http;
    WhatsAppChannel ch(make_webhook_config(), http);

    WebhookRequest req;
    req.method = "PUT";
    req.path   = "/webhook";

    auto resp = ch.handle_webhook_request(req);
    REQUIRE(resp.status == 405);
}

// ── supports_polling ──────────────────────────────────────────────────────────

TEST_CASE("WhatsApp webhook: supports_polling true when webhook_listen set", "[whatsapp_webhook]") {
    MockHttpClient http;
    auto cfg = make_webhook_config();
    cfg.webhook_listen = "127.0.0.1:8080";
    WhatsAppChannel ch(cfg, http);
    REQUIRE(ch.supports_polling());
}

TEST_CASE("WhatsApp webhook: supports_polling false without webhook_listen", "[whatsapp_webhook]") {
    MockHttpClient http;
    auto cfg = make_webhook_config();
    cfg.webhook_listen = "";
    WhatsAppChannel ch(cfg, http);
    REQUIRE_FALSE(ch.supports_polling());
}
