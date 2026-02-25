#include <catch2/catch_test_macros.hpp>
#include "mock_http_client.hpp"
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
