#pragma once
#include "config.hpp"
#include <string>

namespace ptrclaw {

class Provider;

// OAuth token persistence for the OpenAI provider.
//
// Deliberately separate from oauth_openai.hpp: these two are needed whenever the
// OpenAI provider is built, while the interactive authorization flow is optional
// (with_openai_oauth). A build without the flow can still be handed OAuth tokens
// in config, and OpenAIProvider still refreshes them — so the machinery that
// writes rotated tokens back has to come along, or a refresh-token rotation would
// be lost on restart and authentication would fail later for no visible reason.

// Write the OpenAI provider's OAuth fields back to config.json.
bool persist_openai_oauth(const ProviderEntry& entry);

// Install the refresh callback so renewed tokens are written to both the
// in-memory Config and config.json. No-op if the provider is not an
// OpenAIProvider.
void setup_oauth_refresh(Provider* provider, Config& config);

} // namespace ptrclaw
