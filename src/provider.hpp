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
    std::string name;   // display name (e.g. "openai-codex")
    std::string auth;   // "API key", "OAuth", or "local"
    bool active = false;
};

// Returns providers with valid credentials. current_provider and
// current_use_oauth are used to mark the active entry.
std::vector<ProviderInfo> list_providers(
    const Config& config,
    const std::string& current_provider,
    bool current_use_oauth);

// ── Provider switching ──────────────────────────────────────────
struct SwitchProviderResult {
    std::unique_ptr<Provider> provider; // null on error
    std::string model;                  // resolved model name
    std::string display_name;           // e.g. "openai-codex"
    std::string error;                  // non-empty on failure
};

SwitchProviderResult switch_provider(const std::string& name,
                                     const std::string& model_arg,
                                     Config& config,
                                     HttpClient& http);

} // namespace ptrclaw
