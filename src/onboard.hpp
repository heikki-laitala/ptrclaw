#pragma once
#include "config.hpp"
#include "http.hpp"
#include <string>
#include <vector>

namespace ptrclaw {

// Providers hidden from user-facing menus (internal / advanced)
extern const std::vector<std::string> kHiddenProviders;

// Check if a provider is hidden from user-facing menus
bool is_hidden_provider(const std::string& name);

// Human-readable label for a provider name
const char* provider_label(const std::string& name);

// Format auth status for all providers (shared by REPL and channel /auth)
std::string format_auth_status(const Config& config);

// Persist a provider's API key to config.json
bool persist_provider_key(const std::string& provider, const std::string& api_key);

// Returns true if onboarding was completed (at least provider configured).
// hatch_requested is set to true if user wants to hatch after wizard.
bool run_onboard(Config& config, HttpClient& http, bool& hatch_requested);

// Check if onboarding should auto-trigger (no provider has credentials).
bool needs_onboard(const Config& config);

} // namespace ptrclaw
