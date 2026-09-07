#include <catch2/catch_test_macros.hpp>
#include "tools/context_search.hpp"
#include "mock_http_client.hpp"
#include <nlohmann/json.hpp>

using namespace ptrclaw;

namespace {

ContextSearchTool configured(MockHttpClient& http, const std::string& url = "http://ctx/v1/recall") {
    ContextSearchTool tool(&http);
    tool.set_endpoint(url);
    return tool;
}

HttpResponse ok(const std::string& body) {
    HttpResponse r;
    r.status_code = 200;
    r.body = body;
    return r;
}

} // namespace

TEST_CASE("context_search asks the configured endpoint", "[context_search]") {
    MockHttpClient http;
    http.next_response = ok(R"({"memories":[{"content":"Breakfast is 7 until 10."}]})");
    auto tool = configured(http);

    auto res = tool.execute(R"({"query":"when is breakfast"})");

    REQUIRE(res.success);
    REQUIRE(res.output == "Breakfast is 7 until 10.");
    REQUIRE(http.last_url == "http://ctx/v1/recall?q=when%20is%20breakfast");
}

// ⚠ THE QUERY IS MODEL-WRITTEN TEXT. Pasted into a URL unescaped, an "&" splits it into a
// second parameter and the service answers a different question than the model asked.
TEST_CASE("context_search escapes the query", "[context_search]") {
    MockHttpClient http;
    http.next_response = ok(R"({"results":[]})");
    auto tool = configured(http);

    tool.execute(R"({"query":"rooms & rates: \"suite\""})");

    REQUIRE(http.last_url.find("&rates") == std::string::npos);
    REQUIRE(http.last_url.find("q=rooms%20%26%20rates") != std::string::npos);
}

// The shapes a deployment might answer with. Forgiving on purpose — the service is the
// deployment's choice, and a tool that silently returns nothing is how a retrieval path looks
// healthy while delivering nothing.
TEST_CASE("context_search reads the shapes a service might answer with", "[context_search]") {
    struct Case { const char* name; const char* body; const char* want; };
    const Case cases[] = {
        {"object with results",  R"({"results":[{"content":"A"},{"content":"B"}]})", "A\n\nB"},
        {"object with memories", R"({"memories":[{"text":"A"}]})",                   "A"},
        {"object with items",    R"({"items":["A","B"]})",                           "A\n\nB"},
        {"bare array",           R"([{"content":"A"}])",                             "A"},
        // Not a shape it knows: hand it over rather than dropping it. A service that
        // answered something is more use to the model than a tool that says nothing.
        {"unknown shape",        R"({"answer":"A"})",                                R"({"answer":"A"})"},
        {"not json at all",      "just text",                                        "just text"},
    };
    for (const auto& c : cases) {
        MockHttpClient http;
        http.next_response = ok(c.body);
        auto tool = configured(http);
        auto res = tool.execute(R"({"query":"x"})");
        INFO(c.name);
        REQUIRE(res.success);
        REQUIRE(res.output == c.want);
    }
}

// ⚠ "Nothing matched" IS A TRUE ANSWER, not a fault. Reported as a failure the model treats
// it as an error and retries, when what it should do is tell the visitor.
TEST_CASE("context_search reports an empty result as success", "[context_search]") {
    MockHttpClient http;
    http.next_response = ok(R"({"results":[]})");
    auto tool = configured(http);

    auto res = tool.execute(R"({"query":"anything"})");

    REQUIRE(res.success);
    REQUIRE(res.output == "No matching notes.");
}

// ⚠ THE STATUS, NEVER THE BODY. An error body from a service holding other tenants' material
// is not something to hand a model that is talking to a stranger.
TEST_CASE("context_search does not relay the service's error body", "[context_search]") {
    MockHttpClient http;
    HttpResponse r;
    r.status_code = 403;
    r.body = R"({"error":"tenant tnt_other is not permitted"})";
    http.next_response = r;
    auto tool = configured(http);

    auto res = tool.execute(R"({"query":"x"})");

    REQUIRE_FALSE(res.success);
    REQUIRE(res.output.find("403") != std::string::npos);
    REQUIRE(res.output.find("tnt_other") == std::string::npos);
}

TEST_CASE("context_search refuses without an endpoint or a query", "[context_search]") {
    MockHttpClient http;
    ContextSearchTool unconfigured(&http);
    REQUIRE_FALSE(unconfigured.execute(R"({"query":"x"})").success);
    REQUIRE(http.call_count == 0);

    auto tool = configured(http);
    REQUIRE_FALSE(tool.execute(R"({})").success);
    REQUIRE_FALSE(tool.execute(R"({"query":""})").success);
    REQUIRE_FALSE(tool.execute("not json").success);
    REQUIRE(http.call_count == 0);
}

// ⚠ ONE OVERSIZED ANSWER MUST NOT ALLOCATE ITS WAY THROUGH A POD every other session shares —
// the same ceiling, and the same reason, as the scoped file read.
TEST_CASE("context_search bounds what it hands back", "[context_search]") {
    MockHttpClient http;
    nlohmann::json body = {{"results", {{{"content", std::string(80000, 'x')}}}}};
    http.next_response = ok(body.dump());
    auto tool = configured(http);

    auto res = tool.execute(R"({"query":"x"})");

    REQUIRE(res.success);
    REQUIRE(res.output.size() < 60000);
    REQUIRE(res.output.find("[truncated]") != std::string::npos);
}
