#pragma once
#include "../tool.hpp"
#include "../http.hpp"
#include <string>

namespace ptrclaw {

// AcpInvokeTool lets the agent dispatch tasks to an ACP coding agent on demand.
//
// ACP (Agent Communication Protocol) is an open protocol for agent-to-agent
// communication (https://agentcommunicationprotocol.dev/).
//
// Tool parameters:
//   agent    — ACP agent name (required)
//   task     — prompt/task string to send (required)
//   base_url — override ACP server URL (optional; falls back to tools.acp.base_url
//              in config, then http://localhost:8333)
//   api_key  — override bearer token (optional; falls back to tools.acp.api_key)
//   timeout  — request timeout in seconds (optional; default: 600)
//
// The tool POSTs to /runs in synchronous mode and returns the agent text output.
class AcpInvokeTool : public Tool {
public:
    // Construct with config defaults. Per-call args may override base_url/api_key.
    AcpInvokeTool(std::string default_base_url, std::string default_api_key,
                  HttpClient& http);

    ToolResult execute(const std::string& args_json) override;
    std::string tool_name() const override { return "acp_invoke"; }
    std::string description() const override;
    std::string parameters_json() const override;

private:
    std::string default_base_url_;
    std::string default_api_key_;
    HttpClient& http_;

    // Default ACP server endpoint (BeeAI reference implementation default port).
    static constexpr const char* DEFAULT_BASE_URL = "http://localhost:8333";
    // Generous timeout for long-running coding-agent tasks (10 minutes).
    static constexpr long DEFAULT_TIMEOUT_SECONDS = 600;
};

} // namespace ptrclaw
