#pragma once
#include "config.hpp"
#include "http.hpp"

namespace ptrclaw {

// Returns true if onboarding was completed (at least provider configured).
// hatch_requested is set to true if user wants to hatch after wizard.
bool run_onboard(Config& config, HttpClient& http, bool& hatch_requested);

// Check if onboarding should auto-trigger (no provider has credentials).
bool needs_onboard(const Config& config);

} // namespace ptrclaw
