#include "acp_invoke.hpp"
#include "tool_util.hpp"
#include "../config.hpp"
#include "../http.hpp"
#include "../plugin.hpp"
#include <nlohmann/json.hpp>

// Self-register with a static PlatformHttpClient so the tool can make real HTTP
// calls without borrowing the provider's client.
static ptrclaw::ToolRegistrar reg_acp_invoke("acp_invoke", []() {
    auto cfg = ptrclaw::Config::load();
    static ptrclaw::PlatformHttpClient http_instance;
    return std::make_unique<ptrclaw::AcpInvokeTool>(
        cfg.tools_acp.base_url, cfg.tools_acp.api_key, http_instance);
});

using json = nlohmann::json;

namespace {

// Concatenate all text/* parts from an ACP run response output array.
std::string extract_text(const json& response) {
    if (!response.contains("output") || !response["output"].is_array()) return "";
    std::string text;
    for (const auto& msg : response["output"]) {
        if (!msg.contains("parts") || !msg["parts"].is_array()) continue;
        for (const auto& part : msg["parts"]) {
            std::string ct = part.value("content_type", "text/plain");
            if (ct == "text/plain" || ct.rfind("text/", 0) == 0) {
                std::string content = part.value("content", "");
                if (!content.empty()) {
                    if (!text.empty()) text += "\n";
                    text += content;
                }
            }
        }
    }
    return text;
}

} // namespace

namespace ptrclaw {

AcpInvokeTool::AcpInvokeTool(std::string default_base_url,
                               std::string default_api_key,
                               HttpClient& http)
    : default_base_url_(std::move(default_base_url)),
      default_api_key_(std::move(default_api_key)),
      http_(http) {}

std::string AcpInvokeTool::description() const {
    return "Invoke an ACP (Agent Communication Protocol) coding agent on demand. "
           "Sends a task to the specified ACP agent and returns its text output. "
           "Use this to delegate coding, analysis, or other tasks to external agents.";
}

std::string AcpInvokeTool::parameters_json() const {
    return R"json({
  "type": "object",
  "properties": {
    "agent": {
      "type": "string",
      "description": "Name of the ACP agent to invoke (e.g. \"software-engineer\")"
    },
    "task": {
      "type": "string",
      "description": "The task or prompt to send to the agent"
    },
    "base_url": {
      "type": "string",
      "description": "ACP server base URL (overrides tools.acp.base_url from config)"
    },
    "api_key": {
      "type": "string",
      "description": "Bearer token for ACP server auth (overrides tools.acp.api_key from config)"
    },
    "timeout": {
      "type": "integer",
      "description": "Request timeout in seconds (default: 600)"
    }
  },
  "required": ["agent", "task"]
})json";
}

ToolResult AcpInvokeTool::execute(const std::string& args_json) {
    json args;
    if (auto err = parse_tool_json(args_json, args)) return *err;
    if (auto err = require_string(args, "agent")) return *err;
    if (auto err = require_string(args, "task"))  return *err;

    const std::string agent = args["agent"].get<std::string>();
    const std::string task  = args["task"].get<std::string>();

    if (agent.empty()) return ToolResult{false, "Parameter 'agent' must not be empty"};
    if (task.empty())  return ToolResult{false, "Parameter 'task' must not be empty"};

    // Resolve base_url: per-call arg > instance default > hardcoded fallback
    std::string base_url = get_optional_string(args, "base_url", "");
    if (base_url.empty())
        base_url = default_base_url_.empty() ? DEFAULT_BASE_URL : default_base_url_;

    // Resolve api_key: per-call arg > instance default
    const std::string api_key = [&] {
        std::string v = get_optional_string(args, "api_key", "");
        return v.empty() ? default_api_key_ : v;
    }();

    long timeout = DEFAULT_TIMEOUT_SECONDS;
    if (args.contains("timeout") && args["timeout"].is_number_integer()) {
        long t = args["timeout"].get<long>();
        if (t > 0) timeout = t;
    }

    // Build ACP /runs request (sync mode, single user message)
    json request;
    request["agent_name"] = agent;
    request["mode"]       = "sync";
    request["input"]      = json::array({
        json::object({
            {"parts", json::array({
                json::object({{"content_type", "text/plain"}, {"content", task}})
            })}
        })
    });

    std::vector<Header> headers = {
        {"Content-Type", "application/json"},
        {"Accept",       "application/json"},
    };
    if (!api_key.empty()) {
        headers.emplace_back("Authorization", "Bearer " + api_key);
    }

    HttpResponse http_resp;
    try {
        http_resp = http_.post(base_url + "/runs", request.dump(), headers, timeout);
    } catch (const std::exception& e) {
        return ToolResult{false, std::string("ACP request failed: ") + e.what()};
    }

    if (http_resp.status_code < 200 || http_resp.status_code >= 300) {
        return ToolResult{false,
            "ACP server error (HTTP " + std::to_string(http_resp.status_code) +
            "): " + http_resp.body};
    }

    json resp;
    try {
        resp = json::parse(http_resp.body);
    } catch (const std::exception& e) {
        return ToolResult{false, std::string("ACP: failed to parse response: ") + e.what()};
    }

    const std::string status = resp.value("status", "");
    if (status == "failed" || status == "error") {
        const std::string err = resp.contains("error") ? resp["error"].dump() : resp.dump();
        return ToolResult{false, "ACP run failed: " + err};
    }

    const std::string text = extract_text(resp);
    if (text.empty()) {
        return ToolResult{false, "ACP agent returned no text output"};
    }

    return ToolResult{true, text};
}

} // namespace ptrclaw
