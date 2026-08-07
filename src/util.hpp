#pragma once
#include <string>
#include <utility>
#include <vector>
#include <cstdint>

namespace ptrclaw {

// JSON string escaping (for embedding in JSON without nlohmann)
std::string json_escape(const std::string& s);

// Unescape JSON string
std::string json_unescape(const std::string& s);

// ISO 8601 timestamp
std::string timestamp_now();

// Today's UTC date, "2026-08-07". Distinct from timestamp_now() because it goes somewhere
// timestamp_now() must not: the system prompt, which is the front of every request and has
// to stay byte-identical for a provider's prompt cache to match anything behind it.
std::string date_today();

// Unix epoch seconds
uint64_t epoch_seconds();

// Trim whitespace
std::string trim(const std::string& s);

// Lowercase a string
std::string to_lower(const std::string& s);

// Split string by delimiter
std::vector<std::string> split(const std::string& s, char delim);

// `bytes` cryptographically random bytes, lowercase hex.
//
// Distinct from generate_id() on purpose. That one is a thread_local mt19937 and suits an
// id whose only job is correlating a tool batch with its results, where predicting the next
// value buys nothing. This one is for ids that act as capabilities — a serving session id
// selects a private workspace and memory store — where mt19937's recoverable state would
// let one holder derive another's.
std::string secure_random_hex(size_t bytes);

// Simple string replace (all occurrences)
std::string replace_all(const std::string& str, const std::string& from, const std::string& to);

// Generate a simple unique ID (hex)
std::string generate_id();

// FNV-1a 64-bit hash of a byte string. Not cryptographic — used for shard
// selection and for disambiguating sanitized filesystem names.
uint64_t fnv1a(const std::string& s);

// Estimate token count from text (~4 chars per token)
uint32_t estimate_tokens(const std::string& text);

// Expand ~ to home directory
std::string expand_home(const std::string& path);

// Atomic file write: create parent dirs, write to .tmp, rename into place
bool atomic_write_file(const std::string& path, const std::string& content);

// Resolve argv[0] to an absolute binary path (searches PATH if bare name)
std::string resolve_binary_path(const char* argv0);

// Percent-encode a string, leaving RFC 3986 unreserved characters as-is
std::string url_encode(const std::string& s);

// Build an application/x-www-form-urlencoded body from key/value pairs
std::string form_encode(const std::vector<std::pair<std::string, std::string>>& params);

} // namespace ptrclaw
