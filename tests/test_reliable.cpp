#include <catch2/catch_test_macros.hpp>
#include "providers/reliable.hpp"
#include <stdexcept>

using namespace ptrclaw;

// ── Mock provider for testing retry logic ────────────────────────

class FlakyProvider : public Provider {
public:
    int fail_count;     // how many calls should throw before succeeding
    int call_count = 0;
    std::string name;

    FlakyProvider(const std::string& name, int fail_count)
        : fail_count(fail_count), name(name) {}

    ChatResponse chat(const std::vector<ChatMessage>&,
                      const std::vector<ToolSpec>&,
                      const std::string&,
                      double) override {
        call_count++;
        if (call_count <= fail_count) {
            throw std::runtime_error(name + " failed attempt " + std::to_string(call_count));
        }
        ChatResponse resp;
        resp.content = "response from " + name;
        return resp;
    }

    std::string chat_simple(const std::string&,
                            const std::string&,
                            const std::string&,
                            double) override {
        call_count++;
        if (call_count <= fail_count) {
            throw std::runtime_error(name + " simple failed");
        }
        return "simple from " + name;
    }

    bool supports_native_tools() const override { return true; }
    bool supports_streaming() const override { return false; }
    std::string provider_name() const override { return name; }
};

// ── Constructor ──────────────────────────────────────────────────

TEST_CASE("ReliableProvider: requires at least one provider", "[reliable]") {
    std::vector<std::unique_ptr<Provider>> empty;
    REQUIRE_THROWS_AS(
        ReliableProvider(std::move(empty)),
        std::invalid_argument
    );
}

// ── Successful first try ─────────────────────────────────────────

TEST_CASE("ReliableProvider: succeeds on first try", "[reliable]") {
    std::vector<std::unique_ptr<Provider>> providers;
    auto p = std::make_unique<FlakyProvider>("p1", 0);
    auto* ptr = p.get();
    providers.push_back(std::move(p));

    ReliableProvider reliable(std::move(providers), 3);
    auto resp = reliable.chat({}, {}, "model", 0.5);

    REQUIRE(resp.content == "response from p1");
    REQUIRE(ptr->call_count == 1);
}

// ── Retry within same provider ───────────────────────────────────

TEST_CASE("ReliableProvider: retries on failure then succeeds", "[reliable]") {
    std::vector<std::unique_ptr<Provider>> providers;
    auto p = std::make_unique<FlakyProvider>("p1", 2); // fail 2 times, succeed on 3rd
    auto* ptr = p.get();
    providers.push_back(std::move(p));

    ReliableProvider reliable(std::move(providers), 3);
    auto resp = reliable.chat({}, {}, "model", 0.5);

    REQUIRE(resp.content == "response from p1");
    REQUIRE(ptr->call_count == 3);
}

// ── Fallback to second provider ──────────────────────────────────

TEST_CASE("ReliableProvider: falls back to second provider", "[reliable]") {
    std::vector<std::unique_ptr<Provider>> providers;
    auto p1 = std::make_unique<FlakyProvider>("p1", 100); // always fails
    auto p2 = std::make_unique<FlakyProvider>("p2", 0);   // always succeeds
    auto* ptr1 = p1.get();
    auto* ptr2 = p2.get();
    providers.push_back(std::move(p1));
    providers.push_back(std::move(p2));

    ReliableProvider reliable(std::move(providers), 2);
    auto resp = reliable.chat({}, {}, "model", 0.5);

    REQUIRE(resp.content == "response from p2");
    REQUIRE(ptr1->call_count == 2); // exhausted retries
    REQUIRE(ptr2->call_count == 1); // succeeded first try
}

// ── All providers fail ───────────────────────────────────────────

TEST_CASE("ReliableProvider: throws when all providers fail", "[reliable]") {
    std::vector<std::unique_ptr<Provider>> providers;
    providers.push_back(std::make_unique<FlakyProvider>("p1", 100));
    providers.push_back(std::make_unique<FlakyProvider>("p2", 100));

    ReliableProvider reliable(std::move(providers), 2);
    REQUIRE_THROWS_AS(
        reliable.chat({}, {}, "model", 0.5),
        std::runtime_error
    );
}

TEST_CASE("ReliableProvider: error message mentions last error", "[reliable]") {
    std::vector<std::unique_ptr<Provider>> providers;
    providers.push_back(std::make_unique<FlakyProvider>("only", 100));

    ReliableProvider reliable(std::move(providers), 1);
    try {
        reliable.chat({}, {}, "model", 0.5);
        REQUIRE(false); // should not reach here
    } catch (const std::runtime_error& e) {
        std::string msg = e.what();
        REQUIRE(msg.find("All providers failed") != std::string::npos);
    }
}

// ── chat_simple retry logic ──────────────────────────────────────

TEST_CASE("ReliableProvider: chat_simple retries and succeeds", "[reliable]") {
    std::vector<std::unique_ptr<Provider>> providers;
    auto p = std::make_unique<FlakyProvider>("p1", 1);
    providers.push_back(std::move(p));

    ReliableProvider reliable(std::move(providers), 3);
    auto result = reliable.chat_simple("system", "msg", "model", 0.5);
    REQUIRE(result == "simple from p1");
}

TEST_CASE("ReliableProvider: chat_simple throws when all fail", "[reliable]") {
    std::vector<std::unique_ptr<Provider>> providers;
    providers.push_back(std::make_unique<FlakyProvider>("p1", 100));

    ReliableProvider reliable(std::move(providers), 2);
    REQUIRE_THROWS(reliable.chat_simple("s", "m", "model", 0.5));
}

// ── Delegation of capability queries ─────────────────────────────

TEST_CASE("ReliableProvider: delegates supports_native_tools to first", "[reliable]") {
    std::vector<std::unique_ptr<Provider>> providers;
    providers.push_back(std::make_unique<FlakyProvider>("p1", 0));

    ReliableProvider reliable(std::move(providers));
    REQUIRE(reliable.supports_native_tools() == true);
}

TEST_CASE("ReliableProvider: provider_name is reliable", "[reliable]") {
    std::vector<std::unique_ptr<Provider>> providers;
    providers.push_back(std::make_unique<FlakyProvider>("p1", 0));

    ReliableProvider reliable(std::move(providers));
    REQUIRE(reliable.provider_name() == "reliable");
}
