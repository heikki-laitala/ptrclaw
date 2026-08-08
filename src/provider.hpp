#pragma once
#include "tool.hpp"
#include <string>
#include <vector>
#include <optional>
#include <memory>
#include <functional>
#include <cstdint>

namespace ptrclaw {

struct ProviderEntry;

enum class Role { System, User, Assistant, Tool };

inline const char* role_to_string(Role role) {
    switch (role) {
        case Role::System: return "system";
        case Role::User: return "user";
        case Role::Assistant: return "assistant";
        case Role::Tool: return "tool";
    }
    return "user";
}

struct ChatMessage {
    Role role;
    std::string content;
    std::optional<std::string> name;
    std::optional<std::string> tool_call_id;
};

struct ToolCall {
    std::string id;
    std::string name;
    std::string arguments; // raw JSON string
};

struct TokenUsage {
    uint32_t prompt_tokens = 0;
    uint32_t completion_tokens = 0;
    uint32_t total_tokens = 0;
    // How much of prompt_tokens the provider served from its cache, billed at a fraction of
    // a fresh prefix. Zero from a provider that does not report it, which is not the same
    // as a cache that missed — but it is the only distinction available, so treat zero as
    // "no evidence of caching" rather than as proof of none.
    uint32_t cached_prompt_tokens = 0;
};

struct ChatResponse {
    std::optional<std::string> content;
    std::vector<ToolCall> tool_calls;
    TokenUsage usage;
    std::string model;

    bool has_tool_calls() const { return !tool_calls.empty(); }
};

// Callback for streaming text deltas. Return false to abort.
using TextDeltaCallback = std::function<bool(const std::string& delta)>;

// Abstract base class for LLM providers
class Provider {
public:
    virtual ~Provider() = default;

    virtual ChatResponse chat(const std::vector<ChatMessage>& messages,
                              const std::vector<ToolSpec>& tools,
                              const std::string& model,
                              double temperature) = 0;

    virtual std::string chat_simple(const std::string& system_prompt,
                                    const std::string& message,
                                    const std::string& model,
                                    double temperature) = 0;

    virtual ChatResponse chat_stream(const std::vector<ChatMessage>& messages,
                                      const std::vector<ToolSpec>& tools,
                                      const std::string& model,
                                      double temperature,
                                      const TextDeltaCallback& on_delta) {
        (void)on_delta;
        return chat(messages, tools, model, temperature);
    }

    virtual bool supports_native_tools() const = 0;
    virtual bool supports_streaming() const { return false; }
    virtual std::string provider_name() const = 0;
};

class HttpClient; // forward declaration
struct Config;

// Factory: create provider by name
std::unique_ptr<Provider> create_provider(const std::string& name,
                                          const std::string& api_key,
                                          HttpClient& http,
                                          const std::string& base_url = "",
                                          bool prompt_caching = false,
                                          const ProviderEntry* provider_entry = nullptr);

// ── Provider listing ────────────────────────────────────────────
struct ProviderInfo {
    std::string name;
    bool active = false;
    bool has_api_key = false;
    bool has_oauth = false;
    bool is_local = false;
};

std::vector<ProviderInfo> list_providers(
    const Config& config,
    const std::string& current_provider);

// Returns "API key", "OAuth", or "local" based on the active provider + model.
std::string auth_mode_label(const std::string& provider_name,
                             const std::string& model,
                             const Config& config);

// ── OpenAI model routes ─────────────────────────────────────────
// Which transport can serve a model. api.openai.com takes API keys, the ChatGPT backend
// takes subscription tokens, and the two do not serve the same catalog — so the route is
// a property of the model, not of the credential.
enum class OpenAIModelRoute {
    Unknown,          // no first-party route known: treated as API key
    Dual,             // both transports serve it
    PlatformOnly,     // api.openai.com only
    SubscriptionOnly, // ChatGPT backend only
};

OpenAIModelRoute openai_model_route(const std::string& model);

// Whether OpenAI subscription tokens can serve this model: its route reaches the ChatGPT
// backend, or ProviderEntry::oauth_models says so. That list replaces the built-in routes
// rather than extending them, so a deployment can widen or narrow the set.
bool openai_oauth_eligible(const std::string& model, const ProviderEntry& entry);

// ── Provider switching ──────────────────────────────────────────
// For openai, auto-selects OAuth when the model is one the subscription can serve
// (see openai_oauth_eligible) and tokens are present, otherwise uses the API key.
struct SwitchProviderResult {
    std::unique_ptr<Provider> provider; // null on error
    std::string model;                  // resolved model name
    std::string error;                  // non-empty on failure
};

SwitchProviderResult switch_provider(const std::string& name,
                                     const std::string& model_arg,
                                     const std::string& current_model,
                                     Config& config,
                                     HttpClient& http);

} // namespace ptrclaw
