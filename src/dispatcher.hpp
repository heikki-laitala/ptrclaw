#pragma once
#include "provider.hpp"
#include "tool.hpp"
#include <string>
#include <vector>
#include <memory>

namespace ptrclaw {

// Parse tool calls from LLM response text (XML fallback)
std::vector<ToolCall> parse_xml_tool_calls(const std::string& text);

// Try to repair malformed JSON from LLM output
std::string repair_json(const std::string& json_str);

// Execute a single tool call, finding the tool by name
ToolResult dispatch_tool(const ToolCall& call,
                         const std::vector<std::unique_ptr<Tool>>& tools);

// Format tool results as XML for providers that don't support native tools
std::string format_tool_results_xml(const std::string& tool_name,
                                    bool success,
                                    const std::string& output);

// Format tool results as ChatMessage for native tool-call providers
// What a tool result looks like once it is message content: a failure is prefixed so the
// model is told it failed, since role alone does not carry that. Exposed because anything
// exporting a result — the HTTP stream, for one — has to export exactly what history holds,
// or a caller replaying it sends the model something different from what the agent saw.
std::string tool_result_content(bool success, const std::string& output);

ChatMessage format_tool_result_message(const std::string& tool_call_id,
                                       const std::string& tool_name,
                                       bool success,
                                       const std::string& output);

} // namespace ptrclaw
