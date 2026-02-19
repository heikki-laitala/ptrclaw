#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace ptrclaw {

// JSON string escaping (for embedding in JSON without nlohmann)
std::string json_escape(const std::string& s);

// Unescape JSON string
std::string json_unescape(const std::string& s);

// ISO 8601 timestamp
std::string timestamp_now();

// Unix epoch seconds
uint64_t epoch_seconds();

// Trim whitespace
std::string trim(const std::string& s);

// Split string by delimiter
std::vector<std::string> split(const std::string& s, char delim);

// Simple string replace (all occurrences)
std::string replace_all(const std::string& str, const std::string& from, const std::string& to);

// Generate a simple unique ID (hex)
std::string generate_id();

// Estimate token count from text (~4 chars per token)
uint32_t estimate_tokens(const std::string& text);

// Expand ~ to home directory
std::string expand_home(const std::string& path);

} // namespace ptrclaw
