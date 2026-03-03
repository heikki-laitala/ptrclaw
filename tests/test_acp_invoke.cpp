#include <catch2/catch_test_macros.hpp>
#include "mock_http_client.hpp"
#include "tools/acp_invoke.hpp"
#include <nlohmann/json.hpp>

using json = nlohmann::json;
using namespace ptrclaw;

// ── Helpers ────────────────────────────────────────────────────────────────

static std::string find_header(const std::vector<Header>& headers,
                                const std::string& name) {
    for (const auto& h : headers) {
        if (h.first == name) return h.second;
    }
    return "";
}

static const std::string SUCCESS_BODY = R"({
    "run_id": "run-1",
    "status": "completed",
    "output": [{"parts": [{"content_type": "text/plain", "content": "Hello from agent!"}]}]
})";

// ════════════════════════════════════════════════════════════════
// Request shape
// ════════════════════════════════════════════════════════════════

TEST_CASE("acp_invoke: posts to /runs on default base_url", "[tools][acp_invoke]") {
    MockHttpClient mock;
    mock.next_response = {200, SUCCESS_BODY};

    AcpInvokeTool tool("http://localhost:8333", "", mock);
    tool.execute(R"({"agent":"coder","task":"write hello world"})");

    REQUIRE(mock.last_url == "http://localhost:8333/runs");
}

TEST_CASE("acp_invoke: sends correct Content-Type and Accept headers", "[tools][acp_invoke]") {
    MockHttpClient mock;
    mock.next_response = {200, SUCCESS_BODY};

    AcpInvokeTool tool("http://localhost:8333", "", mock);
    tool.execute(R"({"agent":"coder","task":"write hello world"})");

    REQUIRE(find_header(mock.last_headers, "Content-Type") == "application/json");
    REQUIRE(find_header(mock.last_headers, "Accept") == "application/json");
}

TEST_CASE("acp_invoke: request body contains agent_name, mode=sync, and task in input", "[tools][acp_invoke]") {
    MockHttpClient mock;
    mock.next_response = {200, SUCCESS_BODY};

    AcpInvokeTool tool("http://localhost:8333", "", mock);
    tool.execute(R"({"agent":"software-engineer","task":"fix the bug"})");

    auto body = json::parse(mock.last_body);
    REQUIRE(body["agent_name"] == "software-engineer");
    REQUIRE(body["mode"] == "sync");
    REQUIRE(body["input"].is_array());
    REQUIRE(!body["input"].empty());

    // Task text should appear in the first message's first part
    const auto& part = body["input"][0]["parts"][0];
    REQUIRE(part["content"] == "fix the bug");
    REQUIRE(part["content_type"] == "text/plain");
}

TEST_CASE("acp_invoke: sets Bearer token from instance default", "[tools][acp_invoke]") {
    MockHttpClient mock;
    mock.next_response = {200, SUCCESS_BODY};

    AcpInvokeTool tool("http://localhost:8333", "secret-token", mock);
    tool.execute(R"({"agent":"coder","task":"task"})");

    REQUIRE(find_header(mock.last_headers, "Authorization") == "Bearer secret-token");
}

TEST_CASE("acp_invoke: omits Authorization when no api_key", "[tools][acp_invoke]") {
    MockHttpClient mock;
    mock.next_response = {200, SUCCESS_BODY};

    AcpInvokeTool tool("http://localhost:8333", "", mock);
    tool.execute(R"({"agent":"coder","task":"task"})");

    REQUIRE(find_header(mock.last_headers, "Authorization").empty());
}

TEST_CASE("acp_invoke: per-call base_url overrides instance default", "[tools][acp_invoke]") {
    MockHttpClient mock;
    mock.next_response = {200, SUCCESS_BODY};

    AcpInvokeTool tool("http://localhost:8333", "", mock);
    tool.execute(R"({"agent":"coder","task":"task","base_url":"http://remote:9000"})");

    REQUIRE(mock.last_url == "http://remote:9000/runs");
}

TEST_CASE("acp_invoke: per-call api_key overrides instance default", "[tools][acp_invoke]") {
    MockHttpClient mock;
    mock.next_response = {200, SUCCESS_BODY};

    AcpInvokeTool tool("http://localhost:8333", "default-key", mock);
    tool.execute(R"({"agent":"coder","task":"task","api_key":"override-key"})");

    REQUIRE(find_header(mock.last_headers, "Authorization") == "Bearer override-key");
}

TEST_CASE("acp_invoke: uses http://localhost:8333 when no base_url configured", "[tools][acp_invoke]") {
    MockHttpClient mock;
    mock.next_response = {200, SUCCESS_BODY};

    // Empty string default — should fall back to hardcoded default
    AcpInvokeTool tool("", "", mock);
    tool.execute(R"({"agent":"coder","task":"task"})");

    REQUIRE(mock.last_url == "http://localhost:8333/runs");
}

// ════════════════════════════════════════════════════════════════
// Response parsing
// ════════════════════════════════════════════════════════════════

TEST_CASE("acp_invoke: returns agent text output on success", "[tools][acp_invoke]") {
    MockHttpClient mock;
    mock.next_response = {200, SUCCESS_BODY};

    AcpInvokeTool tool("http://localhost:8333", "", mock);
    auto result = tool.execute(R"({"agent":"coder","task":"task"})");

    REQUIRE(result.success);
    REQUIRE(result.output == "Hello from agent!");
}

TEST_CASE("acp_invoke: concatenates multiple output parts with newline", "[tools][acp_invoke]") {
    MockHttpClient mock;
    mock.next_response = {200, R"({
        "status": "completed",
        "output": [{
            "parts": [
                {"content_type": "text/plain", "content": "Part one"},
                {"content_type": "text/plain", "content": "Part two"}
            ]
        }]
    })"};

    AcpInvokeTool tool("http://localhost:8333", "", mock);
    auto result = tool.execute(R"({"agent":"coder","task":"task"})");

    REQUIRE(result.success);
    REQUIRE(result.output == "Part one\nPart two");
}

TEST_CASE("acp_invoke: filters out non-text output parts", "[tools][acp_invoke]") {
    MockHttpClient mock;
    mock.next_response = {200, R"({
        "status": "completed",
        "output": [{
            "parts": [
                {"content_type": "image/png", "content": "binary-data"},
                {"content_type": "text/plain", "content": "actual output"}
            ]
        }]
    })"};

    AcpInvokeTool tool("http://localhost:8333", "", mock);
    auto result = tool.execute(R"({"agent":"coder","task":"task"})");

    REQUIRE(result.success);
    REQUIRE(result.output == "actual output");
}

TEST_CASE("acp_invoke: accepts text/* content types", "[tools][acp_invoke]") {
    MockHttpClient mock;
    mock.next_response = {200, R"({
        "status": "completed",
        "output": [{"parts": [{"content_type": "text/markdown", "content": "# heading"}]}]
    })"};

    AcpInvokeTool tool("http://localhost:8333", "", mock);
    auto result = tool.execute(R"({"agent":"coder","task":"task"})");

    REQUIRE(result.success);
    REQUIRE(result.output == "# heading");
}

// ════════════════════════════════════════════════════════════════
// Error handling
// ════════════════════════════════════════════════════════════════

TEST_CASE("acp_invoke: returns error on HTTP 500", "[tools][acp_invoke]") {
    MockHttpClient mock;
    mock.next_response = {500, "Internal Server Error"};

    AcpInvokeTool tool("http://localhost:8333", "", mock);
    auto result = tool.execute(R"({"agent":"coder","task":"task"})");

    REQUIRE_FALSE(result.success);
    REQUIRE(result.output.find("HTTP 500") != std::string::npos);
}

TEST_CASE("acp_invoke: returns error on HTTP 404", "[tools][acp_invoke]") {
    MockHttpClient mock;
    mock.next_response = {404, "Not Found"};

    AcpInvokeTool tool("http://localhost:8333", "", mock);
    auto result = tool.execute(R"({"agent":"coder","task":"task"})");

    REQUIRE_FALSE(result.success);
    REQUIRE(result.output.find("HTTP 404") != std::string::npos);
}

TEST_CASE("acp_invoke: returns error on failed run status", "[tools][acp_invoke]") {
    MockHttpClient mock;
    mock.next_response = {200, R"({"status":"failed","error":"agent crashed"})"};

    AcpInvokeTool tool("http://localhost:8333", "", mock);
    auto result = tool.execute(R"({"agent":"coder","task":"task"})");

    REQUIRE_FALSE(result.success);
    REQUIRE(result.output.find("ACP run failed") != std::string::npos);
}

TEST_CASE("acp_invoke: returns error on error run status", "[tools][acp_invoke]") {
    MockHttpClient mock;
    mock.next_response = {200, R"({"status":"error","error":"timeout"})"};

    AcpInvokeTool tool("http://localhost:8333", "", mock);
    auto result = tool.execute(R"({"agent":"coder","task":"task"})");

    REQUIRE_FALSE(result.success);
    REQUIRE(result.output.find("ACP run failed") != std::string::npos);
}

TEST_CASE("acp_invoke: returns error on malformed JSON response", "[tools][acp_invoke]") {
    MockHttpClient mock;
    mock.next_response = {200, "not json {"};

    AcpInvokeTool tool("http://localhost:8333", "", mock);
    auto result = tool.execute(R"({"agent":"coder","task":"task"})");

    REQUIRE_FALSE(result.success);
    REQUIRE(result.output.find("failed to parse") != std::string::npos);
}

TEST_CASE("acp_invoke: returns error when agent param missing", "[tools][acp_invoke]") {
    MockHttpClient mock;
    AcpInvokeTool tool("http://localhost:8333", "", mock);
    auto result = tool.execute(R"({"task":"do something"})");

    REQUIRE_FALSE(result.success);
    REQUIRE(result.output.find("agent") != std::string::npos);
    REQUIRE(mock.call_count == 0);  // no HTTP call should be made
}

TEST_CASE("acp_invoke: returns error when task param missing", "[tools][acp_invoke]") {
    MockHttpClient mock;
    AcpInvokeTool tool("http://localhost:8333", "", mock);
    auto result = tool.execute(R"({"agent":"coder"})");

    REQUIRE_FALSE(result.success);
    REQUIRE(result.output.find("task") != std::string::npos);
    REQUIRE(mock.call_count == 0);
}

TEST_CASE("acp_invoke: returns error when agent param is empty string", "[tools][acp_invoke]") {
    MockHttpClient mock;
    AcpInvokeTool tool("http://localhost:8333", "", mock);
    auto result = tool.execute(R"({"agent":"","task":"do something"})");

    REQUIRE_FALSE(result.success);
    REQUIRE(mock.call_count == 0);
}

TEST_CASE("acp_invoke: returns error when output is empty", "[tools][acp_invoke]") {
    MockHttpClient mock;
    mock.next_response = {200, R"({"status":"completed","output":[]})"};

    AcpInvokeTool tool("http://localhost:8333", "", mock);
    auto result = tool.execute(R"({"agent":"coder","task":"task"})");

    REQUIRE_FALSE(result.success);
    REQUIRE(result.output.find("no text output") != std::string::npos);
}

TEST_CASE("acp_invoke: returns error on invalid JSON args", "[tools][acp_invoke]") {
    MockHttpClient mock;
    AcpInvokeTool tool("http://localhost:8333", "", mock);
    auto result = tool.execute("not json");

    REQUIRE_FALSE(result.success);
    REQUIRE(mock.call_count == 0);
}

// ════════════════════════════════════════════════════════════════
// Metadata
// ════════════════════════════════════════════════════════════════

TEST_CASE("acp_invoke: tool_name returns acp_invoke", "[tools][acp_invoke]") {
    MockHttpClient mock;
    AcpInvokeTool tool("", "", mock);
    REQUIRE(tool.tool_name() == "acp_invoke");
}

TEST_CASE("acp_invoke: parameters_json contains required fields", "[tools][acp_invoke]") {
    MockHttpClient mock;
    AcpInvokeTool tool("", "", mock);
    auto schema = json::parse(tool.parameters_json());

    REQUIRE(schema.contains("required"));
    const auto& req = schema["required"];
    bool has_agent = false, has_task = false;
    for (const auto& r : req) {
        if (r == "agent") has_agent = true;
        if (r == "task") has_task = true;
    }
    REQUIRE(has_agent);
    REQUIRE(has_task);
}
