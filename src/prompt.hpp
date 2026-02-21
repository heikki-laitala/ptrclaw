#pragma once
#include "tool.hpp"
#include <string>
#include <vector>
#include <memory>

namespace ptrclaw {

// Build the system prompt, including tool descriptions for XML-based providers.
// When has_memory is true, includes instructions about memory tools and context format.
std::string build_system_prompt(const std::vector<std::unique_ptr<Tool>>& tools,
                                bool include_tool_descriptions,
                                bool has_memory = false);

} // namespace ptrclaw
