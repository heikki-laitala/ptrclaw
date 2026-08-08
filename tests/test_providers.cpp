#include <catch2/catch_test_macros.hpp>
#include "mock_http_client.hpp"
#include "test_helpers.hpp"
#include "providers/openai_token_persist.hpp"
#include "provider.hpp"
#include "providers/anthropic.hpp"
#include "providers/openai.hpp"
#include "providers/ollama.hpp"
#include "providers/openrouter.hpp"
#include "providers/compatible.hpp"
#include <nlohmann/json.hpp>

using json = nlohmann::json;
using namespace ptrclaw;

// ── Helper: find header value ───────────────────────────────────

static std::string find_header(const std::vector<Header>& headers, const std::string& name) {
    for (const auto& h : headers) {
        if (h.first == name) return h.second;
    }
    return "";
}

// ════════════════════════════════════════════════════════════════
// Anthropic Provider
// ════════════════════════════════════════════════════════════════

TEST_CASE("AnthropicProvider: chat sends correct request", "[providers][anthropic]") {
    MockHttpClient mock;
    mock.next_response = {200, R"({
        "model": "claude-3-haiku-20240307",
        "content": [{"type": "text", "text": "Hello!"}],
        "usage": {"input_tokens": 10, "output_tokens": 5}
    })"};

    AnthropicProvider provider("test-key", mock, "");

    std::vector<ChatMessage> messages = {
        {Role::User, "Hi", std::nullopt, std::nullopt}
    };
    auto result = provider.chat(messages, {}, "claude-3-haiku-20240307", 0.7);

    // Verify URL
    REQUIRE(mock.last_url == "https://api.anthropic.com/v1/messages");

    // Verify headers
    REQUIRE(find_header(mock.last_headers, "x-api-key") == "test-key");
    REQUIRE(find_header(mock.last_headers, "anthropic-version") == "2023-06-01");
    REQUIRE(find_header(mock.last_headers, "content-type") == "application/json");

    // Verify request body
    auto body = json::parse(mock.last_body);
    REQUIRE(body["model"] == "claude-3-haiku-20240307");
    REQUIRE(body["temperature"] == 0.7);
    REQUIRE(body["max_tokens"] == 4096);
    REQUIRE(body["messages"].size() == 1);
    REQUIRE(body["messages"][0]["role"] == "user");
    REQUIRE(body["messages"][0]["content"] == "Hi");

    // Verify response parsing
    REQUIRE(result.content.value_or("") == "Hello!");
    REQUIRE(result.model == "claude-3-haiku-20240307");
    REQUIRE(result.usage.prompt_tokens == 10);
    REQUIRE(result.usage.completion_tokens == 5);
    REQUIRE(result.usage.total_tokens == 15);
}

TEST_CASE("AnthropicProvider: chat extracts system messages", "[providers][anthropic]") {
    MockHttpClient mock;
    mock.next_response = {200, R"({
        "model": "claude-3-haiku-20240307",
        "content": [{"type": "text", "text": "ok"}],
        "usage": {"input_tokens": 5, "output_tokens": 2}
    })"};

    AnthropicProvider provider("key", mock, "");

    std::vector<ChatMessage> messages = {
        {Role::System, "Be helpful", std::nullopt, std::nullopt},
        {Role::User, "Hi", std::nullopt, std::nullopt}
    };
    provider.chat(messages, {}, "claude-3-haiku-20240307", 0.5);

    auto body = json::parse(mock.last_body);
    REQUIRE(body["system"] == "Be helpful");
    // System messages should not appear in the messages array
    REQUIRE(body["messages"].size() == 1);
    REQUIRE(body["messages"][0]["role"] == "user");
}

TEST_CASE("AnthropicProvider: chat parses tool calls", "[providers][anthropic]") {
    MockHttpClient mock;
    mock.next_response = {200, R"({
        "model": "claude-3-haiku-20240307",
        "content": [
            {"type": "text", "text": "I'll read the file."},
            {"type": "tool_use", "id": "toolu_123", "name": "file_read", "input": {"path": "/tmp/test.txt"}}
        ],
        "usage": {"input_tokens": 10, "output_tokens": 20}
    })"};

    AnthropicProvider provider("key", mock, "");

    std::vector<ChatMessage> messages = {
        {Role::User, "Read file", std::nullopt, std::nullopt}
    };
    auto result = provider.chat(messages, {}, "claude-3-haiku-20240307", 0.5);

    REQUIRE(result.content.value_or("") == "I'll read the file.");
    REQUIRE(result.has_tool_calls());
    REQUIRE(result.tool_calls.size() == 1);
    REQUIRE(result.tool_calls[0].id == "toolu_123");
    REQUIRE(result.tool_calls[0].name == "file_read");
    auto args = json::parse(result.tool_calls[0].arguments);
    REQUIRE(args["path"] == "/tmp/test.txt");
}

TEST_CASE("AnthropicProvider: chat sends tools in request", "[providers][anthropic]") {
    MockHttpClient mock;
    mock.next_response = {200, R"({
        "model": "claude-3-haiku-20240307",
        "content": [{"type": "text", "text": "ok"}],
        "usage": {"input_tokens": 5, "output_tokens": 2}
    })"};

    AnthropicProvider provider("key", mock, "");

    std::vector<ToolSpec> tools = {
        {"file_read", "Read a file", R"({"type":"object","properties":{"path":{"type":"string"}}})"}
    };
    provider.chat({{Role::User, "Hi", std::nullopt, std::nullopt}}, tools, "claude-3-haiku-20240307", 0.5);

    auto body = json::parse(mock.last_body);
    REQUIRE(body.contains("tools"));
    REQUIRE(body["tools"].size() == 1);
    REQUIRE(body["tools"][0]["name"] == "file_read");
    REQUIRE(body["tools"][0]["description"] == "Read a file");
    REQUIRE(body["tools"][0]["input_schema"]["type"] == "object");
}

TEST_CASE("AnthropicProvider: chat throws on HTTP error", "[providers][anthropic]") {
    MockHttpClient mock;
    mock.next_response = {429, R"({"error": "rate limited"})"};

    AnthropicProvider provider("key", mock, "");

    REQUIRE_THROWS_AS(
        provider.chat({{Role::User, "Hi", std::nullopt, std::nullopt}}, {}, "model", 0.5),
        std::runtime_error);
}

TEST_CASE("AnthropicProvider: chat_simple returns text", "[providers][anthropic]") {
    MockHttpClient mock;
    mock.next_response = {200, R"({
        "model": "claude-3-haiku-20240307",
        "content": [{"type": "text", "text": "Simple answer"}],
        "usage": {"input_tokens": 5, "output_tokens": 3}
    })"};

    AnthropicProvider provider("key", mock, "");
    auto result = provider.chat_simple("Be brief", "What is 2+2?", "claude-3-haiku-20240307", 0.5);
    REQUIRE(result == "Simple answer");

    auto body = json::parse(mock.last_body);
    REQUIRE(body["system"] == "Be brief");
}

TEST_CASE("AnthropicProvider: chat_simple with empty system prompt", "[providers][anthropic]") {
    MockHttpClient mock;
    mock.next_response = {200, R"({
        "model": "model",
        "content": [{"type": "text", "text": "answer"}],
        "usage": {"input_tokens": 5, "output_tokens": 2}
    })"};

    AnthropicProvider provider("key", mock, "");
    provider.chat_simple("", "question", "model", 0.5);

    auto body = json::parse(mock.last_body);
    REQUIRE_FALSE(body.contains("system"));
}

// ════════════════════════════════════════════════════════════════
// OpenAI Provider
// ════════════════════════════════════════════════════════════════

TEST_CASE("OpenAIProvider: chat sends correct request", "[providers][openai]") {
    MockHttpClient mock;
    mock.next_response = {200, R"({
        "model": "gpt-4",
        "choices": [{"message": {"content": "Hello!"}}],
        "usage": {"prompt_tokens": 10, "completion_tokens": 5, "total_tokens": 15}
    })"};

    OpenAIProvider provider("test-key", mock, "");

    std::vector<ChatMessage> messages = {
        {Role::System, "Be helpful", std::nullopt, std::nullopt},
        {Role::User, "Hi", std::nullopt, std::nullopt}
    };
    auto result = provider.chat(messages, {}, "gpt-4", 0.7);

    // Verify URL
    REQUIRE(mock.last_url == "https://api.openai.com/v1/chat/completions");

    // Verify headers
    REQUIRE(find_header(mock.last_headers, "Authorization") == "Bearer test-key");
    REQUIRE(find_header(mock.last_headers, "Content-Type") == "application/json");

    // Verify request body
    auto body = json::parse(mock.last_body);
    REQUIRE(body["model"] == "gpt-4");
    REQUIRE(body["temperature"] == 0.7);
    REQUIRE(body["messages"].size() == 2);
    REQUIRE(body["messages"][0]["role"] == "system");
    REQUIRE(body["messages"][1]["role"] == "user");

    // Verify response parsing
    REQUIRE(result.content.value_or("") == "Hello!");
    REQUIRE(result.model == "gpt-4");
    REQUIRE(result.usage.prompt_tokens == 10);
    REQUIRE(result.usage.completion_tokens == 5);
    REQUIRE(result.usage.total_tokens == 15);
}

TEST_CASE("OpenAIProvider: user is omitted unless set", "[providers][openai]") {
    // Asserted on the body that actually goes over the wire, not on the builder's return
    // value: the builder is protected, and what matters to a gateway counting requests is
    // what arrived. The omission is the compatibility guarantee — a deployment that never
    // sets a user must send exactly the request it sent before, so a provider that
    // rejects unknown fields cannot start failing because this option exists.
    MockHttpClient mock;
    mock.next_response = {200, R"({"choices":[{"message":{"content":"ok"}}]})"};
    OpenAIProvider provider("key", mock, "");
    provider.chat({{Role::User, "Hi", std::nullopt, std::nullopt}}, {}, "gpt-4", 0.7);
    REQUIRE_FALSE(nlohmann::json::parse(mock.last_body).contains("user"));
}

TEST_CASE("OpenAIProvider: user is sent when set", "[providers][openai]") {
    MockHttpClient mock;
    mock.next_response = {200, R"({"choices":[{"message":{"content":"ok"}}]})"};
    OpenAIProvider provider("key", mock, "");
    provider.set_user("pub_ab12");
    provider.chat({{Role::User, "Hi", std::nullopt, std::nullopt}}, {}, "gpt-4", 0.7);
    REQUIRE(nlohmann::json::parse(mock.last_body)["user"] == "pub_ab12");
}

TEST_CASE("OpenAIProvider: user reaches the Responses API request too",
          "[providers][openai]") {
    // Both request shapes or neither. A `user` honoured on one path and dropped on the
    // other is worse than one that does not exist: the caller relying on it for
    // attribution sees some traffic identified and some anonymous, with nothing saying
    // why. A "codex" model is what selects that path (use_responses_api).
    MockHttpClient mock;
    mock.next_response = {200, R"({"output":[{"type":"message","content":[]}]})"};
    OpenAIProvider provider("key", mock, "");
    provider.set_user("pub_ab12");
    provider.chat({{Role::User, "Hi", std::nullopt, std::nullopt}}, {},
                  "gpt-5-codex", 0.7);
    REQUIRE(mock.last_url.find("/responses") != std::string::npos);
    REQUIRE(nlohmann::json::parse(mock.last_body)["user"] == "pub_ab12");
}

TEST_CASE("OpenAIProvider: chat parses tool calls", "[providers][openai]") {
    MockHttpClient mock;
    mock.next_response = {200, R"({
        "model": "gpt-4",
        "choices": [{
            "message": {
                "content": null,
                "tool_calls": [{
                    "id": "call_abc",
                    "type": "function",
                    "function": {
                        "name": "file_read",
                        "arguments": "{\"path\":\"/tmp/test.txt\"}"
                    }
                }]
            }
        }],
        "usage": {"prompt_tokens": 10, "completion_tokens": 15, "total_tokens": 25}
    })"};

    OpenAIProvider provider("key", mock, "");
    auto result = provider.chat({{Role::User, "Read file", std::nullopt, std::nullopt}}, {}, "gpt-4", 0.5);

    REQUIRE_FALSE(result.content.has_value());
    REQUIRE(result.has_tool_calls());
    REQUIRE(result.tool_calls.size() == 1);
    REQUIRE(result.tool_calls[0].id == "call_abc");
    REQUIRE(result.tool_calls[0].name == "file_read");
    auto args = json::parse(result.tool_calls[0].arguments);
    REQUIRE(args["path"] == "/tmp/test.txt");
}

TEST_CASE("OpenAIProvider: chat sends tools in request", "[providers][openai]") {
    MockHttpClient mock;
    mock.next_response = {200, R"({
        "model": "gpt-4",
        "choices": [{"message": {"content": "ok"}}],
        "usage": {"prompt_tokens": 5, "completion_tokens": 2, "total_tokens": 7}
    })"};

    OpenAIProvider provider("key", mock, "");

    std::vector<ToolSpec> tools = {
        {"file_read", "Read a file", R"({"type":"object","properties":{"path":{"type":"string"}}})"}
    };
    provider.chat({{Role::User, "Hi", std::nullopt, std::nullopt}}, tools, "gpt-4", 0.5);

    auto body = json::parse(mock.last_body);
    REQUIRE(body.contains("tools"));
    REQUIRE(body["tools"].size() == 1);
    REQUIRE(body["tools"][0]["type"] == "function");
    REQUIRE(body["tools"][0]["function"]["name"] == "file_read");
}

TEST_CASE("OpenAIProvider: chat throws on HTTP error", "[providers][openai]") {
    MockHttpClient mock;
    mock.next_response = {500, "Internal Server Error"};

    OpenAIProvider provider("key", mock, "");

    REQUIRE_THROWS_AS(
        provider.chat({{Role::User, "Hi", std::nullopt, std::nullopt}}, {}, "gpt-4", 0.5),
        std::runtime_error);
}

TEST_CASE("OpenAIProvider: chat with custom base_url", "[providers][openai]") {
    MockHttpClient mock;
    mock.next_response = {200, R"({
        "model": "gpt-4",
        "choices": [{"message": {"content": "ok"}}],
        "usage": {"prompt_tokens": 5, "completion_tokens": 2, "total_tokens": 7}
    })"};

    OpenAIProvider provider("key", mock, "http://localhost:8080/v1");
    provider.chat({{Role::User, "Hi", std::nullopt, std::nullopt}}, {}, "gpt-4", 0.5);

    REQUIRE(mock.last_url == "http://localhost:8080/v1/chat/completions");
}

TEST_CASE("OpenAIProvider: chat_simple returns text", "[providers][openai]") {
    MockHttpClient mock;
    mock.next_response = {200, R"({
        "model": "gpt-4",
        "choices": [{"message": {"content": "42"}}],
        "usage": {"prompt_tokens": 5, "completion_tokens": 1, "total_tokens": 6}
    })"};

    OpenAIProvider provider("key", mock, "");
    auto result = provider.chat_simple("Be brief", "What is 6*7?", "gpt-4", 0.5);
    REQUIRE(result == "42");
}

// ════════════════════════════════════════════════════════════════
// Ollama Provider
// ════════════════════════════════════════════════════════════════

TEST_CASE("OllamaProvider: chat sends correct request", "[providers][ollama]") {
    MockHttpClient mock;
    mock.next_response = {200, R"({
        "model": "llama3",
        "message": {"content": "Hello from Ollama"},
        "prompt_eval_count": 20,
        "eval_count": 10
    })"};

    OllamaProvider provider(mock, "http://localhost:11434");

    std::vector<ChatMessage> messages = {
        {Role::User, "Hi", std::nullopt, std::nullopt}
    };
    auto result = provider.chat(messages, {}, "llama3", 0.7);

    // Verify URL
    REQUIRE(mock.last_url == "http://localhost:11434/api/chat");

    // Verify headers
    REQUIRE(find_header(mock.last_headers, "Content-Type") == "application/json");

    // Verify request body
    auto body = json::parse(mock.last_body);
    REQUIRE(body["model"] == "llama3");
    REQUIRE(body["stream"] == false);
    REQUIRE(body["messages"].size() == 1);
    REQUIRE(body["messages"][0]["role"] == "user");

    // Verify response parsing
    REQUIRE(result.content.value_or("") == "Hello from Ollama");
    REQUIRE(result.model == "llama3");
    REQUIRE(result.usage.prompt_tokens == 20);
    REQUIRE(result.usage.completion_tokens == 10);
    REQUIRE(result.usage.total_tokens == 30);
}

TEST_CASE("OllamaProvider: does not support native tools", "[providers][ollama]") {
    MockHttpClient mock;
    OllamaProvider provider(mock);
    REQUIRE_FALSE(provider.supports_native_tools());
}

TEST_CASE("OllamaProvider: chat throws on HTTP error", "[providers][ollama]") {
    MockHttpClient mock;
    mock.next_response = {503, "Service Unavailable"};

    OllamaProvider provider(mock);

    REQUIRE_THROWS_AS(
        provider.chat({{Role::User, "Hi", std::nullopt, std::nullopt}}, {}, "llama3", 0.5),
        std::runtime_error);
}

TEST_CASE("OllamaProvider: chat_simple delegates to chat", "[providers][ollama]") {
    MockHttpClient mock;
    mock.next_response = {200, R"({
        "model": "llama3",
        "message": {"content": "Simple answer"}
    })"};

    OllamaProvider provider(mock);
    auto result = provider.chat_simple("System prompt", "Question", "llama3", 0.5);
    REQUIRE(result == "Simple answer");

    // Should have sent system + user messages
    auto body = json::parse(mock.last_body);
    REQUIRE(body["messages"].size() == 2);
    REQUIRE(body["messages"][0]["role"] == "system");
    REQUIRE(body["messages"][1]["role"] == "user");
}

// ════════════════════════════════════════════════════════════════
// OpenRouter Provider
// ════════════════════════════════════════════════════════════════

TEST_CASE("OpenRouterProvider: chat sends correct request with extra headers", "[providers][openrouter]") {
    MockHttpClient mock;
    mock.next_response = {200, R"({
        "model": "anthropic/claude-3-haiku",
        "choices": [{"message": {"content": "Hello!"}}],
        "usage": {"prompt_tokens": 10, "completion_tokens": 5, "total_tokens": 15}
    })"};

    OpenRouterProvider provider("or-key", mock, "");

    std::vector<ChatMessage> messages = {
        {Role::User, "Hi", std::nullopt, std::nullopt}
    };
    auto result = provider.chat(messages, {}, "anthropic/claude-3-haiku", 0.7);

    // Verify URL
    REQUIRE(mock.last_url == "https://openrouter.ai/api/v1/chat/completions");

    // Verify OpenRouter-specific headers
    REQUIRE(find_header(mock.last_headers, "Authorization") == "Bearer or-key");
    REQUIRE(find_header(mock.last_headers, "HTTP-Referer") == "https://ptrclaw.dev");
    REQUIRE(find_header(mock.last_headers, "X-Title") == "PtrClaw");

    // Verify response parsing (OpenAI format)
    REQUIRE(result.content.value_or("") == "Hello!");
    REQUIRE(result.usage.prompt_tokens == 10);
}

TEST_CASE("OpenRouterProvider: chat parses tool calls", "[providers][openrouter]") {
    MockHttpClient mock;
    mock.next_response = {200, R"({
        "model": "model",
        "choices": [{
            "message": {
                "content": null,
                "tool_calls": [{
                    "id": "call_xyz",
                    "type": "function",
                    "function": {"name": "shell", "arguments": "{\"command\":\"ls\"}"}
                }]
            }
        }],
        "usage": {"prompt_tokens": 5, "completion_tokens": 10, "total_tokens": 15}
    })"};

    OpenRouterProvider provider("key", mock, "");
    auto result = provider.chat({{Role::User, "Run ls", std::nullopt, std::nullopt}}, {}, "model", 0.5);

    REQUIRE(result.has_tool_calls());
    REQUIRE(result.tool_calls[0].name == "shell");
}

TEST_CASE("OpenRouterProvider: chat_simple returns text", "[providers][openrouter]") {
    MockHttpClient mock;
    mock.next_response = {200, R"({
        "model": "model",
        "choices": [{"message": {"content": "42"}}],
        "usage": {"prompt_tokens": 5, "completion_tokens": 1, "total_tokens": 6}
    })"};

    OpenRouterProvider provider("key", mock, "");
    auto result = provider.chat_simple("Be brief", "What is 6*7?", "model", 0.5);
    REQUIRE(result == "42");

    auto body = json::parse(mock.last_body);
    REQUIRE(body["messages"].size() == 2);
    REQUIRE(body["messages"][0]["role"] == "system");
}

TEST_CASE("OpenRouterProvider: chat sends tools in request", "[providers][openrouter]") {
    MockHttpClient mock;
    mock.next_response = {200, R"({
        "model": "model",
        "choices": [{"message": {"content": "ok"}}],
        "usage": {"prompt_tokens": 5, "completion_tokens": 2, "total_tokens": 7}
    })"};

    OpenRouterProvider provider("key", mock, "");

    std::vector<ToolSpec> tools = {
        {"shell", "Run a shell command", R"({"type":"object","properties":{"command":{"type":"string"}}})"}
    };
    provider.chat({{Role::User, "Hi", std::nullopt, std::nullopt}}, tools, "model", 0.5);

    auto body = json::parse(mock.last_body);
    REQUIRE(body.contains("tools"));
    REQUIRE(body["tools"].size() == 1);
    REQUIRE(body["tools"][0]["type"] == "function");
    REQUIRE(body["tools"][0]["function"]["name"] == "shell");
}

TEST_CASE("OpenRouterProvider: chat round-trips assistant tool calls", "[providers][openrouter]") {
    MockHttpClient mock;
    mock.next_response = {200, R"({
        "model": "model",
        "choices": [{"message": {"content": "done"}}],
        "usage": {"prompt_tokens": 10, "completion_tokens": 5, "total_tokens": 15}
    })"};

    OpenRouterProvider provider("key", mock, "");

    // Simulate an assistant message with tool calls serialized in name field
    std::string tool_calls_json = R"([{"id":"call_1","name":"shell","arguments":"{\"command\":\"ls\"}"}])";
    std::vector<ChatMessage> messages = {
        {Role::User, "Run ls", std::nullopt, std::nullopt},
        {Role::Assistant, "I'll run that.", std::string(tool_calls_json), std::nullopt},
        {Role::Tool, "file1.txt\nfile2.txt", std::nullopt, std::string("call_1")},
    };
    provider.chat(messages, {}, "model", 0.5);

    auto body = json::parse(mock.last_body);
    // Assistant message should have tool_calls
    bool found_assistant_tc = false;
    bool found_tool = false;
    for (const auto& msg : body["messages"]) {
        if (msg["role"] == "assistant" && msg.contains("tool_calls")) {
            found_assistant_tc = true;
            REQUIRE(msg["tool_calls"].size() == 1);
            REQUIRE(msg["tool_calls"][0]["function"]["name"] == "shell");
        }
        if (msg["role"] == "tool") {
            found_tool = true;
            REQUIRE(msg["tool_call_id"] == "call_1");
        }
    }
    REQUIRE(found_assistant_tc);
    REQUIRE(found_tool);
}

TEST_CASE("OpenRouterProvider: chat throws on HTTP error", "[providers][openrouter]") {
    MockHttpClient mock;
    mock.next_response = {502, "Bad Gateway"};

    OpenRouterProvider provider("key", mock, "");

    REQUIRE_THROWS_AS(
        provider.chat({{Role::User, "Hi", std::nullopt, std::nullopt}}, {}, "model", 0.5),
        std::runtime_error);
}

// ════════════════════════════════════════════════════════════════
// Compatible Provider
// ════════════════════════════════════════════════════════════════

TEST_CASE("CompatibleProvider: uses custom base URL", "[providers][compatible]") {
    MockHttpClient mock;
    mock.next_response = {200, R"({
        "model": "local-model",
        "choices": [{"message": {"content": "ok"}}],
        "usage": {"prompt_tokens": 5, "completion_tokens": 2, "total_tokens": 7}
    })"};

    CompatibleProvider provider("key", mock, "http://my-server:9000/v1");
    provider.chat({{Role::User, "Hi", std::nullopt, std::nullopt}}, {}, "local-model", 0.5);

    REQUIRE(mock.last_url == "http://my-server:9000/v1/chat/completions");
}

TEST_CASE("CompatibleProvider: provider_name is compatible", "[providers][compatible]") {
    MockHttpClient mock;
    CompatibleProvider provider("key", mock, "http://localhost:8080");
    REQUIRE(provider.provider_name() == "compatible");
}

// ════════════════════════════════════════════════════════════════
// Tool result round-tripping (Anthropic)
// ════════════════════════════════════════════════════════════════

TEST_CASE("AnthropicProvider: sends tool results as user message", "[providers][anthropic]") {
    MockHttpClient mock;
    mock.next_response = {200, R"({
        "model": "model",
        "content": [{"type": "text", "text": "ok"}],
        "usage": {"input_tokens": 5, "output_tokens": 2}
    })"};

    AnthropicProvider provider("key", mock, "");

    std::vector<ChatMessage> messages = {
        {Role::User, "Read file", std::nullopt, std::nullopt},
        {Role::Tool, "file contents here", std::nullopt, std::string("toolu_123")}
    };
    provider.chat(messages, {}, "model", 0.5);

    auto body = json::parse(mock.last_body);
    // Tool results should be wrapped in a user message with tool_result content blocks
    bool found_tool_result = false;
    for (const auto& msg : body["messages"]) {
        if (msg.contains("content") && msg["content"].is_array()) {
            for (const auto& block : msg["content"]) {
                if (block.value("type", "") == "tool_result") {
                    found_tool_result = true;
                    REQUIRE(block["tool_use_id"] == "toolu_123");
                    REQUIRE(block["content"] == "file contents here");
                }
            }
        }
    }
    REQUIRE(found_tool_result);
}

// ════════════════════════════════════════════════════════════════
// Tool result round-tripping (OpenAI)
// ════════════════════════════════════════════════════════════════

TEST_CASE("OpenAIProvider: sends tool results with tool_call_id", "[providers][openai]") {
    MockHttpClient mock;
    mock.next_response = {200, R"({
        "model": "gpt-4",
        "choices": [{"message": {"content": "ok"}}],
        "usage": {"prompt_tokens": 5, "completion_tokens": 2, "total_tokens": 7}
    })"};

    OpenAIProvider provider("key", mock, "");

    std::vector<ChatMessage> messages = {
        {Role::User, "Read file", std::nullopt, std::nullopt},
        {Role::Tool, "file contents", std::nullopt, std::string("call_abc")}
    };
    provider.chat(messages, {}, "gpt-4", 0.5);

    auto body = json::parse(mock.last_body);
    bool found_tool = false;
    for (const auto& msg : body["messages"]) {
        if (msg["role"] == "tool") {
            found_tool = true;
            REQUIRE(msg["tool_call_id"] == "call_abc");
            REQUIRE(msg["content"] == "file contents");
        }
    }
    REQUIRE(found_tool);
}

// ════════════════════════════════════════════════════════════════
// Edge cases: empty/malformed responses
// ════════════════════════════════════════════════════════════════

TEST_CASE("AnthropicProvider: empty content array returns no content", "[providers][anthropic]") {
    MockHttpClient mock;
    mock.next_response = {200, R"({
        "model": "model",
        "content": [],
        "usage": {"input_tokens": 5, "output_tokens": 0}
    })"};

    AnthropicProvider provider("key", mock, "");
    auto result = provider.chat({{Role::User, "Hi", std::nullopt, std::nullopt}}, {}, "model", 0.5);

    REQUIRE_FALSE(result.content.has_value());
    REQUIRE_FALSE(result.has_tool_calls());
}

TEST_CASE("OpenAIProvider: empty choices returns no content", "[providers][openai]") {
    MockHttpClient mock;
    mock.next_response = {200, R"({
        "model": "gpt-4",
        "choices": [],
        "usage": {"prompt_tokens": 5, "completion_tokens": 0, "total_tokens": 5}
    })"};

    OpenAIProvider provider("key", mock, "");
    auto result = provider.chat({{Role::User, "Hi", std::nullopt, std::nullopt}}, {}, "gpt-4", 0.5);

    REQUIRE_FALSE(result.content.has_value());
    REQUIRE_FALSE(result.has_tool_calls());
}

// ════════════════════════════════════════════════════════════════
// OpenAI Provider: OAuth
// ════════════════════════════════════════════════════════════════

TEST_CASE("OpenAIProvider: uses Bearer token from OAuth when use_oauth is true", "[providers][openai][oauth]") {
    MockHttpClient mock;
    mock.next_response = {200, R"({
        "model": "gpt-4",
        "choices": [{"message": {"content": "ok"}}],
        "usage": {"prompt_tokens": 5, "completion_tokens": 2, "total_tokens": 7}
    })"};

    // Token expires far in the future so no refresh needed
    OpenAIProvider provider("api-key", mock, "",
                            true, "my-oauth-token", "my-refresh", 9999999999);

    provider.chat({{Role::User, "Hi", std::nullopt, std::nullopt}}, {}, "gpt-4", 0.5);

    REQUIRE(find_header(mock.last_headers, "Authorization") == "Bearer my-oauth-token");
}

TEST_CASE("OpenAIProvider: refresh_oauth_if_needed refreshes expired token", "[providers][openai][oauth]") {
    MockHttpClient mock;
    // First call: refresh endpoint returns new token
    mock.response_queue.push_back({200, R"({
        "access_token": "new-access-token",
        "refresh_token": "new-refresh-token",
        "expires_in": 3600
    })"});
    // Second call: chat endpoint
    mock.response_queue.push_back({200, R"({
        "model": "gpt-4",
        "choices": [{"message": {"content": "ok"}}],
        "usage": {"prompt_tokens": 5, "completion_tokens": 2, "total_tokens": 7}
    })"});

    // Token already expired (epoch 1)
    OpenAIProvider provider("api-key", mock, "",
                            true, "old-token", "my-refresh", 1,
                            "test-client", "https://auth.test/token");

    provider.chat({{Role::User, "Hi", std::nullopt, std::nullopt}}, {}, "gpt-4", 0.5);

    // The chat call should use the refreshed token
    REQUIRE(find_header(mock.last_headers, "Authorization") == "Bearer new-access-token");
    REQUIRE(mock.call_count == 2);
}

TEST_CASE("OpenAIProvider: throws when token expired and no refresh token", "[providers][openai][oauth]") {
    MockHttpClient mock;

    // Token expired, no refresh token
    OpenAIProvider provider("api-key", mock, "",
                            true, "expired-token", "", 1);

    REQUIRE_THROWS_AS(
        provider.chat({{Role::User, "Hi", std::nullopt, std::nullopt}}, {}, "gpt-4", 0.5),
        std::runtime_error);
}

TEST_CASE("OpenAIProvider: on_token_refresh callback fires after refresh", "[providers][openai][oauth]") {
    MockHttpClient mock;
    mock.response_queue.push_back({200, R"({
        "access_token": "refreshed-token",
        "refresh_token": "rotated-refresh",
        "expires_in": 7200
    })"});
    mock.response_queue.push_back({200, R"({
        "model": "gpt-4",
        "choices": [{"message": {"content": "ok"}}],
        "usage": {"prompt_tokens": 5, "completion_tokens": 2, "total_tokens": 7}
    })"});

    OpenAIProvider provider("api-key", mock, "",
                            true, "old-token", "old-refresh", 1,
                            "client-id", "https://auth.test/token");

    std::string cb_access, cb_refresh;
    uint64_t cb_expires = 0;
    provider.set_on_token_refresh([&](const std::string& at, const std::string& rt, uint64_t ea) {
        cb_access = at;
        cb_refresh = rt;
        cb_expires = ea;
    });

    provider.chat({{Role::User, "Hi", std::nullopt, std::nullopt}}, {}, "gpt-4", 0.5);

    REQUIRE(cb_access == "refreshed-token");
    REQUIRE(cb_refresh == "rotated-refresh");
    REQUIRE(cb_expires > 0);
}

// ════════════════════════════════════════════════════════════════
// OpenAI Provider: Responses API (codex models)
// ════════════════════════════════════════════════════════════════

TEST_CASE("OpenAIProvider: codex model hits /responses endpoint", "[providers][openai][responses]") {
    MockHttpClient mock;
    mock.next_response = {200, R"({
        "model": "gpt-5-codex-mini",
        "output": [
            {"type": "message", "content": [{"type": "output_text", "text": "Hello from codex!"}]}
        ],
        "usage": {"input_tokens": 8, "output_tokens": 4}
    })"};

    OpenAIProvider provider("test-key", mock, "");

    std::vector<ChatMessage> messages = {
        {Role::System, "Be helpful", std::nullopt, std::nullopt},
        {Role::User, "Hi", std::nullopt, std::nullopt}
    };
    auto result = provider.chat(messages, {}, "gpt-5-codex-mini", 0.7);

    // Verify URL is /responses, not /chat/completions
    REQUIRE(mock.last_url == "https://api.openai.com/v1/responses");

    // Verify request format uses "input" + "instructions" (not "messages")
    auto body = json::parse(mock.last_body);
    REQUIRE(body.contains("input"));
    REQUIRE(body.contains("instructions"));
    REQUIRE_FALSE(body.contains("messages"));
    REQUIRE(body["instructions"] == "Be helpful");
    REQUIRE(body["model"] == "gpt-5-codex-mini");

    // Input should have only the user message (system extracted to instructions)
    REQUIRE(body["input"].size() == 1);
    REQUIRE(body["input"][0]["role"] == "user");

    // Verify response parsing
    REQUIRE(result.content.value_or("") == "Hello from codex!");
    REQUIRE(result.model == "gpt-5-codex-mini");
    REQUIRE(result.usage.prompt_tokens == 8);
    REQUIRE(result.usage.completion_tokens == 4);
    REQUIRE(result.usage.total_tokens == 12);
}

TEST_CASE("OpenAIProvider: non-codex model stays on Chat Completions", "[providers][openai][responses]") {
    MockHttpClient mock;
    mock.next_response = {200, R"({
        "model": "gpt-4",
        "choices": [{"message": {"content": "Hello!"}}],
        "usage": {"prompt_tokens": 10, "completion_tokens": 5, "total_tokens": 15}
    })"};

    OpenAIProvider provider("key", mock, "");
    provider.chat({{Role::User, "Hi", std::nullopt, std::nullopt}}, {}, "gpt-4", 0.5);

    REQUIRE(mock.last_url == "https://api.openai.com/v1/chat/completions");
}

TEST_CASE("OpenAIProvider: Responses API parses tool calls", "[providers][openai][responses]") {
    MockHttpClient mock;
    mock.next_response = {200, R"({
        "model": "gpt-5-codex-mini",
        "output": [
            {"type": "function_call", "call_id": "call_123", "name": "file_read", "arguments": "{\"path\":\"/tmp/test.txt\"}"}
        ],
        "usage": {"input_tokens": 12, "output_tokens": 8}
    })"};

    OpenAIProvider provider("key", mock, "");
    auto result = provider.chat(
        {{Role::User, "Read file", std::nullopt, std::nullopt}}, {}, "gpt-5-codex-mini", 0.5);

    REQUIRE(result.has_tool_calls());
    REQUIRE(result.tool_calls.size() == 1);
    REQUIRE(result.tool_calls[0].id == "call_123");
    REQUIRE(result.tool_calls[0].name == "file_read");
    auto args = json::parse(result.tool_calls[0].arguments);
    REQUIRE(args["path"] == "/tmp/test.txt");
}

TEST_CASE("OpenAIProvider: Responses API tool results sent as function_call_output", "[providers][openai][responses]") {
    MockHttpClient mock;
    mock.next_response = {200, R"({
        "model": "gpt-5-codex",
        "output": [
            {"type": "message", "content": [{"type": "output_text", "text": "Got it."}]}
        ],
        "usage": {"input_tokens": 20, "output_tokens": 3}
    })"};

    OpenAIProvider provider("key", mock, "");

    std::vector<ChatMessage> messages = {
        {Role::User, "Read file", std::nullopt, std::nullopt},
        {Role::Tool, "file contents here", std::nullopt, std::string("call_123")}
    };
    provider.chat(messages, {}, "gpt-5-codex", 0.5);

    auto body = json::parse(mock.last_body);
    // Should have function_call_output item in input
    bool found_output = false;
    for (const auto& item : body["input"]) {
        if (item.value("type", "") == "function_call_output") {
            found_output = true;
            REQUIRE(item["call_id"] == "call_123");
            REQUIRE(item["output"] == "file contents here");
        }
    }
    REQUIRE(found_output);
}

TEST_CASE("OpenAIProvider: Responses API tools use flat format", "[providers][openai][responses]") {
    MockHttpClient mock;
    mock.next_response = {200, R"({
        "model": "gpt-5-codex-mini",
        "output": [
            {"type": "message", "content": [{"type": "output_text", "text": "ok"}]}
        ],
        "usage": {"input_tokens": 5, "output_tokens": 2}
    })"};

    OpenAIProvider provider("key", mock, "");

    std::vector<ToolSpec> tools = {
        {"file_read", "Read a file", R"({"type":"object","properties":{"path":{"type":"string"}}})"}
    };
    provider.chat({{Role::User, "Hi", std::nullopt, std::nullopt}}, tools, "gpt-5-codex-mini", 0.5);

    auto body = json::parse(mock.last_body);
    REQUIRE(body.contains("tools"));
    REQUIRE(body["tools"].size() == 1);
    // Flat format: name at top level (not nested under "function")
    REQUIRE(body["tools"][0]["type"] == "function");
    REQUIRE(body["tools"][0]["name"] == "file_read");
    REQUIRE(body["tools"][0]["description"] == "Read a file");
    REQUIRE(body["tools"][0]["parameters"]["type"] == "object");
    REQUIRE_FALSE(body["tools"][0].contains("function"));
}

TEST_CASE("OpenAIProvider: chat_simple with codex model uses Responses API", "[providers][openai][responses]") {
    MockHttpClient mock;
    mock.next_response = {200, R"({
        "model": "gpt-5-codex-mini",
        "output": [
            {"type": "message", "content": [{"type": "output_text", "text": "42"}]}
        ],
        "usage": {"input_tokens": 5, "output_tokens": 1}
    })"};

    OpenAIProvider provider("key", mock, "");
    auto result = provider.chat_simple("Be brief", "What is 6*7?", "gpt-5-codex-mini", 0.5);
    REQUIRE(result == "42");

    // Should hit /responses
    REQUIRE(mock.last_url == "https://api.openai.com/v1/responses");

    // System prompt → instructions
    auto body = json::parse(mock.last_body);
    REQUIRE(body["instructions"] == "Be brief");
    REQUIRE_FALSE(body.contains("messages"));
}

// ════════════════════════════════════════════════════════════════
// OpenAI Provider: OAuth routing for non-codex models
// ════════════════════════════════════════════════════════════════

TEST_CASE("OpenAIProvider: OAuth sends non-codex model to the ChatGPT backend",
          "[providers][openai][oauth][responses]") {
    MockHttpClient mock;
    mock.next_response = {200, R"({
        "model": "gpt-5.5",
        "output": [
            {"type": "message", "content": [{"type": "output_text", "text": "Hi from gpt-5.5"}]}
        ],
        "usage": {"input_tokens": 6, "output_tokens": 3}
    })"};

    OpenAIProvider provider("api-key", mock, "",
                            true, "oauth-token", "refresh", 9999999999);

    auto result = provider.chat(
        {{Role::System, "Be helpful", std::nullopt, std::nullopt},
         {Role::User, "Hi", std::nullopt, std::nullopt}}, {}, "gpt-5.5", 0.5);

    // The subscription backend only speaks the Responses API, whatever the model.
    REQUIRE(mock.last_url == "https://chatgpt.com/backend-api/codex/responses");
    REQUIRE(find_header(mock.last_headers, "Authorization") == "Bearer oauth-token");

    auto body = json::parse(mock.last_body);
    REQUIRE(body.contains("input"));
    REQUIRE(body["instructions"] == "Be helpful");
    REQUIRE_FALSE(body.contains("messages"));
    REQUIRE(result.content.value_or("") == "Hi from gpt-5.5");
}

TEST_CASE("OpenAIProvider: api key sends a dual-route model to Responses",
          "[providers][openai][responses]") {
    MockHttpClient mock;
    mock.next_response = {200, R"({
        "model": "gpt-5.6-sol",
        "output": [{"type": "message", "content": [{"type": "output_text", "text": "ok"}]}],
        "usage": {"input_tokens": 4, "output_tokens": 1}
    })"};

    OpenAIProvider provider("api-key", mock, "");
    auto result = provider.chat({{Role::User, "Hi", std::nullopt, std::nullopt}}, {},
                                "gpt-5.6-sol", 0.5);

    // No "codex" in the id, no OAuth — the route table is what knows this is a
    // Responses model on api.openai.com.
    REQUIRE(mock.last_url == "https://api.openai.com/v1/responses");
    auto body = json::parse(mock.last_body);
    REQUIRE(body.contains("input"));
    REQUIRE_FALSE(body.contains("messages"));
    REQUIRE(result.content.value_or("") == "ok");
}

TEST_CASE("OpenAIProvider: api key sends a platform-only model to Responses",
          "[providers][openai][responses]") {
    MockHttpClient mock;
    mock.next_response = {200, R"({
        "model": "gpt-5.6",
        "output": [{"type": "message", "content": [{"type": "output_text", "text": "ok"}]}],
        "usage": {"input_tokens": 4, "output_tokens": 1}
    })"};

    OpenAIProvider provider("api-key", mock, "");
    provider.chat({{Role::User, "Hi", std::nullopt, std::nullopt}}, {}, "gpt-5.6", 0.5);

    REQUIRE(mock.last_url == "https://api.openai.com/v1/responses");
}

TEST_CASE("OpenAIProvider: api key keeps an unknown model on Chat Completions",
          "[providers][openai][responses]") {
    MockHttpClient mock;
    mock.next_response = {200, R"({
        "model": "gpt-5",
        "choices": [{"message": {"content": "Hello!"}}],
        "usage": {"prompt_tokens": 10, "completion_tokens": 5, "total_tokens": 15}
    })"};

    OpenAIProvider provider("api-key", mock, "");
    auto result = provider.chat({{Role::User, "Hi", std::nullopt, std::nullopt}}, {}, "gpt-5", 0.5);

    REQUIRE(mock.last_url == "https://api.openai.com/v1/chat/completions");
    REQUIRE(result.content.value_or("") == "Hello!");
}

// A base_url override means the caller is pointing at something other than OpenAI's
// subscription backend, so the ChatGPT path must not be forced onto it.
TEST_CASE("OpenAIProvider: OAuth with base_url override keeps Chat Completions",
          "[providers][openai][oauth]") {
    MockHttpClient mock;
    mock.next_response = {200, R"({
        "model": "gpt-5",
        "choices": [{"message": {"content": "proxied"}}],
        "usage": {"prompt_tokens": 4, "completion_tokens": 1, "total_tokens": 5}
    })"};

    OpenAIProvider provider("api-key", mock, "https://proxy.test/v1",
                            true, "oauth-token", "refresh", 9999999999);
    provider.chat({{Role::User, "Hi", std::nullopt, std::nullopt}}, {}, "gpt-5", 0.5);

    REQUIRE(mock.last_url == "https://proxy.test/v1/chat/completions");
    REQUIRE(find_header(mock.last_headers, "Authorization") == "Bearer oauth-token");
}

TEST_CASE("OpenAIProvider: codex model on a base_url override stays on that host",
          "[providers][openai][oauth][responses]") {
    MockHttpClient mock;
    mock.next_response = {200, R"({
        "model": "gpt-5-codex",
        "output": [
            {"type": "message", "content": [{"type": "output_text", "text": "ok"}]}
        ],
        "usage": {"input_tokens": 4, "output_tokens": 1}
    })"};

    OpenAIProvider provider("api-key", mock, "https://proxy.test/v1",
                            true, "oauth-token", "refresh", 9999999999);
    provider.chat({{Role::User, "Hi", std::nullopt, std::nullopt}}, {}, "gpt-5-codex", 0.5);

    REQUIRE(mock.last_url == "https://proxy.test/v1/responses");
}

TEST_CASE("OpenAIProvider: a ChatGPT base_url resolves to the codex responses endpoint",
          "[providers][openai][oauth][responses]") {
    MockHttpClient mock;
    mock.next_response = {200, R"({
        "model": "gpt-5.5",
        "output": [
            {"type": "message", "content": [{"type": "output_text", "text": "ok"}]}
        ],
        "usage": {"input_tokens": 4, "output_tokens": 1}
    })"};

    OpenAIProvider provider("api-key", mock, "https://chatgpt.com/backend-api/codex",
                            true, "oauth-token", "refresh", 9999999999);
    provider.chat({{Role::User, "Hi", std::nullopt, std::nullopt}}, {}, "gpt-5.5", 0.5);

    REQUIRE(mock.last_url == "https://chatgpt.com/backend-api/codex/responses");
}

// That host speaks nothing but Responses, so the endpoint decides the shape even when an
// API key is what authenticates.
TEST_CASE("OpenAIProvider: a ChatGPT base_url uses Responses without OAuth",
          "[providers][openai][responses]") {
    MockHttpClient mock;
    mock.next_response = {200, R"({
        "model": "gpt-5.5",
        "output": [
            {"type": "message", "content": [{"type": "output_text", "text": "ok"}]}
        ],
        "usage": {"input_tokens": 4, "output_tokens": 1}
    })"};

    OpenAIProvider provider("api-key", mock, "https://chatgpt.com/backend-api/codex/responses");
    provider.chat({{Role::User, "Hi", std::nullopt, std::nullopt}}, {}, "gpt-5.5", 0.5);

    REQUIRE(mock.last_url == "https://chatgpt.com/backend-api/codex/responses");
    auto body = json::parse(mock.last_body);
    REQUIRE(body.contains("input"));
    REQUIRE_FALSE(body.contains("messages"));
}

// ════════════════════════════════════════════════════════════════
// OpenAI Provider: ChatGPT backend request identity
// ════════════════════════════════════════════════════════════════

// Payload claims {"https://api.openai.com/auth":{"chatgpt_account_id":"acct_test_123",...}}
static const char* kTokenWithAccountId =
    "eyJhbGciOiAiUlMyNTYiLCAidHlwIjogIkpXVCJ9."
    "eyJodHRwczovL2FwaS5vcGVuYWkuY29tL2F1dGgiOiB7ImNoYXRncHRfYWNjb3VudF9pZCI6ICJhY2N0X3Rlc3RfMTIz"
    "IiwgImNoYXRncHRfcGxhbl90eXBlIjogInBybyJ9fQ.sig";
// Same shape, claim absent.
static const char* kTokenWithoutAccountId =
    "eyJhbGciOiAiUlMyNTYiLCAidHlwIjogIkpXVCJ9."
    "eyJodHRwczovL2FwaS5vcGVuYWkuY29tL2F1dGgiOiB7ImNoYXRncHRfcGxhbl90eXBlIjogInBybyJ9fQ.sig";

TEST_CASE("openai_account_id_from_token: reads the ChatGPT auth claim",
          "[providers][openai][oauth]") {
    REQUIRE(openai_account_id_from_token(kTokenWithAccountId) == "acct_test_123");
}

TEST_CASE("openai_account_id_from_token: anything else yields nothing",
          "[providers][openai][oauth]") {
    REQUIRE(openai_account_id_from_token(kTokenWithoutAccountId).empty());
    REQUIRE(openai_account_id_from_token("").empty());
    REQUIRE(openai_account_id_from_token("not-a-jwt").empty());
    REQUIRE(openai_account_id_from_token("a.b.c").empty());
    REQUIRE(openai_account_id_from_token("aaaa.bbbb").empty());
}

// The backend routes a subscription request by account, so a token covering more than one
// workspace needs it. It comes from the live access token, not from config, so it follows
// the token across a refresh.
TEST_CASE("OpenAIProvider: ChatGPT backend requests identify the account",
          "[providers][openai][oauth]") {
    MockHttpClient mock;
    mock.next_response = {200, R"({
        "model": "gpt-5.5",
        "output": [{"type": "message", "content": [{"type": "output_text", "text": "ok"}]}],
        "usage": {"input_tokens": 4, "output_tokens": 1}
    })"};

    OpenAIProvider provider("api-key", mock, "",
                            true, kTokenWithAccountId, "refresh", 9999999999);
    provider.chat({{Role::User, "Hi", std::nullopt, std::nullopt}}, {}, "gpt-5.5", 0.5);

    REQUIRE(find_header(mock.last_headers, "chatgpt-account-id") == "acct_test_123");
    REQUIRE(find_header(mock.last_headers, "originator") == "pi");
    REQUIRE(find_header(mock.last_headers, "User-Agent").rfind("pi", 0) == 0);
}

TEST_CASE("OpenAIProvider: a token without the claim sends no account header",
          "[providers][openai][oauth]") {
    MockHttpClient mock;
    mock.next_response = {200, R"({
        "model": "gpt-5.5",
        "output": [{"type": "message", "content": [{"type": "output_text", "text": "ok"}]}],
        "usage": {"input_tokens": 4, "output_tokens": 1}
    })"};

    OpenAIProvider provider("api-key", mock, "",
                            true, kTokenWithoutAccountId, "refresh", 9999999999);
    provider.chat({{Role::User, "Hi", std::nullopt, std::nullopt}}, {}, "gpt-5.5", 0.5);

    REQUIRE(find_header(mock.last_headers, "chatgpt-account-id").empty());
}

TEST_CASE("OpenAIProvider: platform requests carry no ChatGPT identity",
          "[providers][openai]") {
    MockHttpClient mock;
    mock.next_response = {200, R"({
        "model": "gpt-4",
        "choices": [{"message": {"content": "ok"}}],
        "usage": {"prompt_tokens": 5, "completion_tokens": 2, "total_tokens": 7}
    })"};

    OpenAIProvider provider("api-key", mock, "");
    provider.chat({{Role::User, "Hi", std::nullopt, std::nullopt}}, {}, "gpt-4", 0.5);

    REQUIRE(find_header(mock.last_headers, "chatgpt-account-id").empty());
    REQUIRE(find_header(mock.last_headers, "originator").empty());
}

// ════════════════════════════════════════════════════════════════
// OpenAI Provider: token endpoint guards
// ════════════════════════════════════════════════════════════════

// A refresh token is a long-lived credential; there is no configuration in which sending
// it over plaintext is the intended behavior.
TEST_CASE("OpenAIProvider: refresh refuses a plaintext token endpoint",
          "[providers][openai][oauth]") {
    MockHttpClient mock;
    OpenAIProvider provider("api-key", mock, "",
                            true, "old-token", "my-refresh", 1,
                            "test-client", "http://auth.test/token");

    REQUIRE_THROWS_AS(
        provider.chat({{Role::User, "Hi", std::nullopt, std::nullopt}}, {}, "gpt-4", 0.5),
        std::runtime_error);
    REQUIRE(mock.call_count == 0);
}

TEST_CASE("OpenAIProvider: refresh rejects an oversized token response",
          "[providers][openai][oauth]") {
    MockHttpClient mock;
    mock.response_queue.push_back({200, std::string(kOAuthTokenBodyLimitBytes + 1, 'x')});

    OpenAIProvider provider("api-key", mock, "",
                            true, "old-token", "my-refresh", 1,
                            "test-client", "https://auth.test/token");

    REQUIRE_THROWS_AS(
        provider.chat({{Role::User, "Hi", std::nullopt, std::nullopt}}, {}, "gpt-4", 0.5),
        std::runtime_error);
    REQUIRE(mock.call_count == 1);
}

// The non-2xx branch puts the body in the exception message, which becomes the user's
// chat reply — so the cap has to be checked before it, not after.
TEST_CASE("OpenAIProvider: refresh does not put an oversized error body in the message",
          "[providers][openai][oauth]") {
    MockHttpClient mock;
    mock.response_queue.push_back({502, std::string(kOAuthTokenBodyLimitBytes + 1, 'x')});

    OpenAIProvider provider("api-key", mock, "",
                            true, "old-token", "my-refresh", 1,
                            "test-client", "https://auth.test/token");

    bool threw = false;
    try {
        provider.chat({{Role::User, "Hi", std::nullopt, std::nullopt}}, {}, "gpt-4", 0.5);
    } catch (const std::runtime_error& e) {
        threw = true;
        REQUIRE(std::string(e.what()).size() < kOAuthTokenBodyLimitBytes);
    }
    REQUIRE(threw);
}

TEST_CASE("OpenAIProvider: token refresh does not wait out the chat timeout",
          "[providers][openai][oauth]") {
    MockHttpClient mock;
    mock.response_queue.push_back({200, R"({
        "access_token": "new-access-token",
        "refresh_token": "new-refresh-token",
        "expires_in": 3600
    })"});
    mock.response_queue.push_back({200, R"({
        "model": "gpt-4",
        "choices": [{"message": {"content": "ok"}}],
        "usage": {"prompt_tokens": 5, "completion_tokens": 2, "total_tokens": 7}
    })"});

    OpenAIProvider provider("api-key", mock, "",
                            true, "old-token", "my-refresh", 1,
                            "test-client", "https://auth.test/token");
    provider.chat({{Role::User, "Hi", std::nullopt, std::nullopt}}, {}, "gpt-4", 0.5);

    REQUIRE(mock.timeouts.size() == 2);
    REQUIRE(mock.timeouts[0] == 30);
}

// ── OAuth token persistence ──────────────────────────────────────
//
// persist_openai_oauth() and setup_oauth_refresh() are compiled whenever the
// OpenAI provider is built, independently of the interactive OAuth flow, because a
// deployment can be handed tokens in config and the provider still refreshes them.
// These tests run under HomeGuard: config code resolves "~" through $HOME, so the
// writes land in a temp dir rather than the developer's own config.

TEST_CASE("persist_openai_oauth: writes the OAuth fields to the config file", "[providers][oauth]") {
    HomeGuard home;
    home.write_default_config();

    ProviderEntry entry;
    entry.use_oauth = true;
    entry.oauth_access_token = "at-1";
    entry.oauth_refresh_token = "rt-1";
    entry.oauth_expires_at = 1234567890;
    entry.oauth_client_id = "client-abc";
    entry.oauth_token_url = "https://example.test/token";

    REQUIRE(persist_openai_oauth(entry));

    auto j = home.read_config();
    const auto& o = j["providers"]["openai"];
    REQUIRE(o["use_oauth"] == true);
    REQUIRE(o["oauth_access_token"] == "at-1");
    REQUIRE(o["oauth_refresh_token"] == "rt-1");
    REQUIRE(o["oauth_expires_at"] == 1234567890);
    REQUIRE(o["oauth_client_id"] == "client-abc");
    REQUIRE(o["oauth_token_url"] == "https://example.test/token");
}

TEST_CASE("setup_oauth_refresh: a refresh is written back to config and file", "[providers][oauth]") {
    // The failure this pins is delayed and silent: without the callback the
    // provider still refreshes, but the ROTATED refresh token is never stored, so
    // the next start loads a stale one and authentication fails later.
    HomeGuard home;
    home.write_default_config();

    Config cfg;
    cfg.providers["openai"].use_oauth = true;
    cfg.providers["openai"].oauth_access_token = "old-at";
    cfg.providers["openai"].oauth_refresh_token = "old-rt";
    cfg.providers["openai"].oauth_expires_at = 1; // long expired

    MockHttpClient mock;
    // Refresh endpoint first, then the chat call that triggered it — matching the
    // existing refresh test, since refresh_oauth_if_needed() is protected and only
    // reachable through a real request.
    mock.response_queue.push_back({200, R"({
        "access_token": "new-at",
        "refresh_token": "new-rt",
        "expires_in": 3600
    })"});
    mock.response_queue.push_back({200, R"({
        "model": "gpt-4",
        "choices": [{"message": {"content": "ok"}}],
        "usage": {"prompt_tokens": 5, "completion_tokens": 2, "total_tokens": 7}
    })"});

    OpenAIProvider provider("", mock, "",
                            true, "old-at", "old-rt", 1,
                            "client-abc", "https://example.test/token");
    setup_oauth_refresh(&provider, cfg);

    provider.chat({{Role::User, "Hi", std::nullopt, std::nullopt}}, {}, "gpt-4", 0.5);

    // In-memory Config updated...
    REQUIRE(cfg.providers["openai"].oauth_access_token == "new-at");
    REQUIRE(cfg.providers["openai"].oauth_refresh_token == "new-rt");
    // ...and written through to the file, which is the part that survives restart.
    auto j = home.read_config();
    REQUIRE(j["providers"]["openai"]["oauth_access_token"] == "new-at");
    REQUIRE(j["providers"]["openai"]["oauth_refresh_token"] == "new-rt");
}

// ── The `user` field reaches every OpenAI-dialect provider ──────
//
// providers.openai.user identifies the caller to the endpoint, and a gateway can require
// it — hirebell-llm answers 400 missing_user without one. Only the openai factory forwarded
// it, so pointing the compatible or openrouter provider at such a gateway failed every call
// while the config plainly set the field.

TEST_CASE("create_provider: compatible forwards the configured user", "[providers]") {
    REQUIRE_TEST_PROVIDER("compatible");
    MockHttpClient mock;
    mock.next_response = {200, R"({
        "model": "gw-model",
        "choices": [{"message": {"content": "ok"}}],
        "usage": {"prompt_tokens": 1, "completion_tokens": 1, "total_tokens": 2}
    })"};

    ProviderEntry entry;
    entry.api_key = "test-key";
    entry.user = "pub_demo123";
    auto p = create_provider("compatible", entry.api_key, mock, "http://gw.local/v1",
                             false, &entry);
    REQUIRE(p != nullptr);
    p->chat({{Role::User, "hi", std::nullopt, std::nullopt}}, {}, "gw-model", 0.7);

    auto body = json::parse(mock.last_body);
    REQUIRE(body.contains("user"));
    REQUIRE(body["user"] == "pub_demo123");
}

TEST_CASE("create_provider: openrouter forwards the configured user", "[providers]") {
    REQUIRE_TEST_PROVIDER("openrouter");
    MockHttpClient mock;
    mock.next_response = {200, R"({
        "model": "openai/gpt-4o",
        "choices": [{"message": {"content": "ok"}}],
        "usage": {"prompt_tokens": 1, "completion_tokens": 1, "total_tokens": 2}
    })"};

    ProviderEntry entry;
    entry.api_key = "or-key";
    entry.user = "pub_demo123";
    auto p = create_provider("openrouter", entry.api_key, mock, "", false, &entry);
    REQUIRE(p != nullptr);
    p->chat({{Role::User, "hi", std::nullopt, std::nullopt}}, {}, "openai/gpt-4o", 0.7);

    auto body = json::parse(mock.last_body);
    REQUIRE(body.contains("user"));
    REQUIRE(body["user"] == "pub_demo123");
}

TEST_CASE("create_provider: no configured user sends no user field", "[providers]") {
    // The field is omitted rather than sent empty: an endpoint that validates it should see
    // a missing key, which is what its own error message is about.
    REQUIRE_TEST_PROVIDER("compatible");
    MockHttpClient mock;
    mock.next_response = {200, R"({
        "model": "gw-model",
        "choices": [{"message": {"content": "ok"}}],
        "usage": {"prompt_tokens": 1, "completion_tokens": 1, "total_tokens": 2}
    })"};

    ProviderEntry entry;
    entry.api_key = "test-key";
    auto p = create_provider("compatible", entry.api_key, mock, "http://gw.local/v1",
                             false, &entry);
    REQUIRE(p != nullptr);
    p->chat({{Role::User, "hi", std::nullopt, std::nullopt}}, {}, "gw-model", 0.7);

    REQUIRE_FALSE(json::parse(mock.last_body).contains("user"));
}

// ── Cached prompt tokens ────────────────────────────────────────
//
// A provider bills a cached prefix at a fraction of a fresh one, and the pod goes to some
// trouble to keep that prefix byte-stable. Unparsed, whether any of it works is invisible:
// a change that quietly breaks the prefix shows up as a bill rather than a signal.

TEST_CASE("OpenAIProvider: reads cached tokens from chat completions usage",
          "[providers][openai]") {
    // gpt-4 on purpose: use_responses_api() dispatches on the *model*, and a subscription
    // model would take the Responses path, which spells the usage block differently. That
    // path is covered below — both matter, since a pod pointed at a gateway takes chat
    // completions while a subscription login takes Responses.
    REQUIRE_TEST_PROVIDER("openai");
    MockHttpClient mock;
    mock.next_response = {200, R"({
        "model": "gpt-4",
        "choices": [{"message": {"content": "hi"}}],
        "usage": {"prompt_tokens": 1200, "completion_tokens": 5, "total_tokens": 1205,
                  "prompt_tokens_details": {"cached_tokens": 1024}}
    })"};

    OpenAIProvider provider("sk-test", mock, "http://localhost:8080/v1");
    auto result = provider.chat({{Role::User, "hi", std::nullopt, std::nullopt}}, {},
                                "gpt-4", 0.7);

    REQUIRE(result.usage.prompt_tokens == 1200);
    REQUIRE(result.usage.cached_prompt_tokens == 1024);
}

TEST_CASE("OpenAIProvider: reads cached tokens from the Responses API usage",
          "[providers][openai]") {
    // The subscription path, where the parent key is input_tokens_details.
    REQUIRE_TEST_PROVIDER("openai");
    MockHttpClient mock;
    mock.next_response = {200, R"({
        "model": "gpt-5.6-sol",
        "output": [{"type": "message", "role": "assistant",
                    "content": [{"type": "output_text", "text": "hi"}]}],
        "usage": {"input_tokens": 1200, "output_tokens": 5,
                  "input_tokens_details": {"cached_tokens": 1024}}
    })"};

    OpenAIProvider provider("sk-test", mock, "https://chatgpt.com/backend-api/codex");
    auto result = provider.chat({{Role::User, "hi", std::nullopt, std::nullopt}}, {},
                                "gpt-5.6-sol", 0.7);

    REQUIRE(result.usage.prompt_tokens == 1200);
    REQUIRE(result.usage.cached_prompt_tokens == 1024);
}

TEST_CASE("OpenAIProvider: absent cache details read as zero, not as an error",
          "[providers][openai]") {
    // An endpoint that does not report caching is not an endpoint that failed.
    REQUIRE_TEST_PROVIDER("openai");
    MockHttpClient mock;
    mock.next_response = {200, R"({
        "model": "gpt-4",
        "choices": [{"message": {"content": "hi"}}],
        "usage": {"prompt_tokens": 40, "completion_tokens": 5, "total_tokens": 45}
    })"};

    OpenAIProvider provider("sk-test", mock, "http://localhost:8080/v1");
    auto result = provider.chat({{Role::User, "hi", std::nullopt, std::nullopt}}, {},
                                "gpt-4", 0.7);

    REQUIRE(result.usage.prompt_tokens == 40);
    REQUIRE(result.usage.cached_prompt_tokens == 0);
}

TEST_CASE("AnthropicProvider: cache tokens count toward the prompt total",
          "[providers][anthropic]") {
    // Anthropic's input_tokens counts only what was neither read from nor written to the
    // cache — unlike OpenAI, where the cached figure is a subset of prompt_tokens. Reported
    // as-is, a 1,954-token prompt arrives as 30 and the cached count exceeds the prompt it
    // is supposedly part of.
    REQUIRE_TEST_PROVIDER("anthropic");
    MockHttpClient mock;
    mock.next_response = {200, R"({
        "model": "claude-sonnet-4-6",
        "content": [{"type": "text", "text": "hi"}],
        "usage": {"input_tokens": 30, "output_tokens": 5,
                  "cache_creation_input_tokens": 900,
                  "cache_read_input_tokens": 1024}
    })"};

    AnthropicProvider provider("sk-test", mock, "");
    auto result = provider.chat({{Role::User, "hi", std::nullopt, std::nullopt}}, {},
                                "claude-sonnet-4-6", 0.7);

    REQUIRE(result.usage.prompt_tokens == 1954);   // 30 fresh + 900 written + 1024 read
    REQUIRE(result.usage.cached_prompt_tokens == 1024);  // the read alone is the saving
    REQUIRE(result.usage.total_tokens == 1959);
}

TEST_CASE("AnthropicProvider: streaming reports cache usage too",
          "[providers][anthropic]") {
    // The path an agent actually takes, since this provider streams. Parsing only the
    // non-streaming site left ordinary turns reporting no caching at all.
    REQUIRE_TEST_PROVIDER("anthropic");
    MockHttpClient mock;
    mock.next_stream_body =
        "event: message_start\n"
        "data: {\"type\":\"message_start\",\"message\":{\"model\":\"claude-sonnet-4-6\","
        "\"usage\":{\"input_tokens\":30,\"cache_creation_input_tokens\":900,"
        "\"cache_read_input_tokens\":1024}}}\n\n"
        "event: content_block_delta\n"
        "data: {\"type\":\"content_block_delta\",\"index\":0,"
        "\"delta\":{\"type\":\"text_delta\",\"text\":\"hi\"}}\n\n"
        "event: message_delta\n"
        "data: {\"type\":\"message_delta\",\"usage\":{\"output_tokens\":5}}\n\n"
        "event: message_stop\n"
        "data: {\"type\":\"message_stop\"}\n\n";

    AnthropicProvider provider("sk-test", mock, "");
    auto result = provider.chat_stream({{Role::User, "hi", std::nullopt, std::nullopt}}, {},
                                       "claude-sonnet-4-6", 0.7,
                                       [](const std::string&) { return true; });

    REQUIRE(result.usage.prompt_tokens == 1954);
    REQUIRE(result.usage.cached_prompt_tokens == 1024);
}
