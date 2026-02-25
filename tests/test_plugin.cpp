#include <catch2/catch_test_macros.hpp>
#include "mock_http_client.hpp"
#include "plugin.hpp"

using namespace ptrclaw;

// Tests use unique prefixed names to avoid colliding with real registrations.
// We never call clear() on the global singleton since it would destroy
// the static registrations from provider/tool/channel .cpp files.

// ── Helpers ─────────────────────────────────────────────────────

class PluginTestProvider : public Provider {
public:
    std::string name_;
    PluginTestProvider(const std::string& name) : name_(name) {}
    ChatResponse chat(const std::vector<ChatMessage>&,
                      const std::vector<ToolSpec>&,
                      const std::string&, double) override {
        return {};
    }
    std::string chat_simple(const std::string&, const std::string&,
                            const std::string&, double) override { return ""; }
    bool supports_native_tools() const override { return true; }
    std::string provider_name() const override { return name_; }
};

class PluginTestTool : public Tool {
public:
    std::string name_;
    PluginTestTool(const std::string& name) : name_(name) {}
    ToolResult execute(const std::string&) override { return {true, "ok"}; }
    std::string tool_name() const override { return name_; }
    std::string description() const override { return "test"; }
    std::string parameters_json() const override { return R"({"type":"object"})"; }
};

class PluginTestChannel : public Channel {
public:
    std::string name_;
    PluginTestChannel(const std::string& name) : name_(name) {}
    std::string channel_name() const override { return name_; }
    bool health_check() override { return true; }
    void send_message(const std::string&, const std::string&) override {}
};

// ── Built-in static registrations ───────────────────────────────

TEST_CASE("PluginRegistry: at least one provider is registered", "[plugin]") {
    auto names = PluginRegistry::instance().provider_names();
    REQUIRE_FALSE(names.empty());
}

// ── Provider registration & creation ────────────────────────────

TEST_CASE("PluginRegistry: register and create custom provider", "[plugin]") {
    auto& reg = PluginRegistry::instance();

    reg.register_provider("_test_prov", [](const std::string&, HttpClient&, const std::string&,
                                             bool) {
        return std::make_unique<PluginTestProvider>("_test_prov");
    });

    REQUIRE(reg.has_provider("_test_prov"));

    MockHttpClient http;
    auto provider = reg.create_provider("_test_prov", "", http, "", false);
    REQUIRE(provider != nullptr);
    REQUIRE(provider->provider_name() == "_test_prov");
}

TEST_CASE("PluginRegistry: create unknown provider throws", "[plugin]") {
    MockHttpClient http;
    REQUIRE_THROWS_AS(
        PluginRegistry::instance().create_provider("_nonexistent_prov", "", http, "", false),
        std::invalid_argument);
}

TEST_CASE("PluginRegistry: provider_names returns sorted list", "[plugin]") {
    auto names = PluginRegistry::instance().provider_names();
    REQUIRE_FALSE(names.empty());
    // Verify sorted
    for (size_t i = 1; i < names.size(); i++) {
        REQUIRE(names[i - 1] <= names[i]);
    }
}

// ── Tool registration ───────────────────────────────────────────

TEST_CASE("PluginRegistry: register and create custom tool", "[plugin]") {
    auto& reg = PluginRegistry::instance();

    reg.register_tool("_test_tool", []() {
        return std::make_unique<PluginTestTool>("_test_tool");
    });

    auto names = reg.tool_names();
    bool found = false;
    for (const auto& n : names) {
        if (n == "_test_tool") found = true;
    }
    REQUIRE(found);
}

// ── Channel registration ────────────────────────────────────────

TEST_CASE("PluginRegistry: register and create custom channel", "[plugin]") {
    auto& reg = PluginRegistry::instance();

    reg.register_channel("_test_ch", [](const Config&, HttpClient&) {
        return std::make_unique<PluginTestChannel>("_test_ch");
    });

    REQUIRE(reg.has_channel("_test_ch"));

    Config cfg;
    MockHttpClient http;
    auto ch = reg.create_channel("_test_ch", cfg, http);
    REQUIRE(ch != nullptr);
    REQUIRE(ch->channel_name() == "_test_ch");
}

TEST_CASE("PluginRegistry: create unknown channel throws", "[plugin]") {
    Config cfg;
    MockHttpClient http;
    REQUIRE_THROWS_AS(
        PluginRegistry::instance().create_channel("_nonexistent_ch", cfg, http),
        std::invalid_argument);
}

// ── Registrar structs ───────────────────────────────────────────

TEST_CASE("PluginRegistry: ProviderRegistrar auto-registers", "[plugin]") {
    ProviderRegistrar reg("_auto_prov", [](const std::string&, HttpClient&, const std::string&,
                                             bool) {
        return std::make_unique<PluginTestProvider>("_auto_prov");
    });
    REQUIRE(PluginRegistry::instance().has_provider("_auto_prov"));
}

TEST_CASE("PluginRegistry: ToolRegistrar auto-registers", "[plugin]") {
    ToolRegistrar reg("_auto_tool", []() {
        return std::make_unique<PluginTestTool>("_auto_tool");
    });
    auto names = PluginRegistry::instance().tool_names();
    bool found = false;
    for (const auto& n : names) {
        if (n == "_auto_tool") found = true;
    }
    REQUIRE(found);
}

TEST_CASE("PluginRegistry: ChannelRegistrar auto-registers", "[plugin]") {
    ChannelRegistrar reg("_auto_ch", [](const Config&, HttpClient&) {
        return std::make_unique<PluginTestChannel>("_auto_ch");
    });
    REQUIRE(PluginRegistry::instance().has_channel("_auto_ch"));
}

// ── create_all_tools creates working instances ──────────────────

TEST_CASE("PluginRegistry: create_all_tools returns tool instances", "[plugin]") {
    auto tools = PluginRegistry::instance().create_all_tools();
    for (const auto& t : tools) {
        REQUIRE_FALSE(t->tool_name().empty());
    }
}
