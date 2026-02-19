#pragma once
#include "tool.hpp"
#include <string>
#include <vector>
#include <optional>
#include <memory>
#include <cstdint>

namespace ptrclaw {

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

    virtual bool supports_native_tools() const = 0;
    virtual bool supports_streaming() const { return false; }
    virtual std::string provider_name() const = 0;
};

class HttpClient; // forward declaration

// Factory: create provider by name
std::unique_ptr<Provider> create_provider(const std::string& name,
                                          const std::string& api_key,
                                          HttpClient& http,
                                          const std::string& base_url = "");

} // namespace ptrclaw
