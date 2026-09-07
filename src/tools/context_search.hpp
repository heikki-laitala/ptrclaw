#pragma once
#include "../tool.hpp"
#include "../http.hpp"
#include <string>

namespace ptrclaw {

// Search a shared knowledge service over HTTP for material this session was not given.
//
// The counterpart to the scoped file tools: those read what a context manager staged on disk
// before the session began, this asks a service a question the session has only now thought
// of. A pushed context is decided before the model has read the visitor's message, so it
// cannot answer what the model works out mid-turn that it needs — a note that mentions a
// separate price list, say.
//
// ⚠ IT HOLDS NO CREDENTIAL. The URL comes from `serving.recall_url` and nothing else; a
// deployment that needs authentication puts it on the proxy the pod's egress already passes
// through, the same way a model-provider key is injected. The agent process never holds it,
// so a prompt injection cannot read one out of its own configuration.
//
// ⚠ AND IT NAMES NO PRODUCT. `?q=` in, JSON out. What serves it is the deployment's business.
class ContextSearchTool : public RecallAwareTool {
public:
    ContextSearchTool() = default;
    // Injectable so this is testable without a socket; null means the platform client.
    explicit ContextSearchTool(HttpClient* client) : client_(client) {}

    ToolResult execute(const std::string& args_json) override;
    std::string tool_name() const override { return "context_search"; }
    std::string description() const override;
    std::string parameters_json() const override;

private:
    HttpClient* client_ = nullptr;
};

} // namespace ptrclaw
