#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "channels/telegram.hpp"
#include "event.hpp"
#include "event_bus.hpp"
#include "memory.hpp"
#include "mock_http_client.hpp"
#include "plugin.hpp"
#include "session.hpp"

using namespace ptrclaw;
using json = nlohmann::json;

TEST_CASE("Integration: Telegram message reaches provider with memory context block", "[integration][telegram][memory]") {
    // 1) Simulate inbound Telegram update
    MockHttpClient telegram_http;
    telegram_http.next_response = {200, R"({
        "ok": true,
        "result": [{
            "update_id": 100,
            "message": {
                "message_id": 1,
                "from": {"id": 42, "username": "alice", "first_name": "Alice"},
                "chat": {"id": 12345, "type": "private"},
                "date": 1700000000,
                "text": "what do you remember about my pet?"
            }
        }]
    })"};

    TelegramConfig tg_cfg;
    tg_cfg.bot_token = "test-token";
    tg_cfg.allow_from = {"*"};
    TelegramChannel tg(tg_cfg, telegram_http);

    auto messages = tg.poll_updates();
    REQUIRE(messages.size() == 1);

    // 2) Session pipeline with mocked provider HTTP
    MockHttpClient provider_http;
    provider_http.next_response = {
        200,
        R"({"model":"llama3","message":{"content":"ok"},"prompt_eval_count":10,"eval_count":2})"
    };

    Config cfg;
    cfg.provider = "ollama"; // non-streaming provider => uses mocked post()
    cfg.model = "llama3";
    cfg.providers["ollama"].base_url = "http://localhost:11434";
    cfg.memory.backend = "json";
    cfg.memory.path = "/tmp/ptrclaw_test_memory_integration.json";
    cfg.memory.recall_limit = 5;
    cfg.memory.enrich_depth = 0;
    cfg.memory.auto_save = false;
    cfg.agent.max_tool_iterations = 3;

    SessionManager mgr(cfg, provider_http);
    EventBus bus;
    mgr.set_event_bus(&bus);
    mgr.subscribe_events();

    // Seed memory for the same session before handling incoming message.
    auto& agent = mgr.get_session("telegram:test-session");
    REQUIRE(agent.memory() != nullptr);
    // Avoid auto-hatching mode so normal memory enrichment is used.
    agent.memory()->store("soul:identity", "Name: Test Assistant", MemoryCategory::Core, "telegram:test-session");
    agent.memory()->store("pet", "Your pet's name is Milo", MemoryCategory::Knowledge, "telegram:test-session");

    // Optional capture of assistant reply to ensure full event path executed.
    std::string assistant_reply;
    subscribe<MessageReadyEvent>(bus, [&](const MessageReadyEvent& ev) {
        assistant_reply = ev.content;
    });

    MessageReceivedEvent in;
    in.session_id = "telegram:test-session";
    in.message = messages[0];
    bus.publish(in);

    REQUIRE_FALSE(assistant_reply.empty());
    REQUIRE(provider_http.call_count >= 1);

    // 3) Inspect outgoing provider payload: user message must contain memory block
    auto body = json::parse(provider_http.last_body);
    REQUIRE(body.contains("messages"));
    REQUIRE(body["messages"].is_array());

    bool found_memory_block = false;
    for (const auto& m : body["messages"]) {
        if (!m.contains("role") || !m.contains("content")) continue;
        if (m["role"] != "user") continue;
        if (!m["content"].is_string()) continue;

        std::string content = m["content"].get<std::string>();
        if (content.find("[Memory context]") != std::string::npos &&
            content.find("pet: Your pet's name is Milo") != std::string::npos &&
            content.find("[/Memory context]") != std::string::npos) {
            found_memory_block = true;
            break;
        }
    }

    REQUIRE(found_memory_block);
}
