#pragma once
#include <string>

namespace ptrclaw {

class Agent;
class HttpClient;
struct Config;

// Shared command handlers used by both REPL (main.cpp) and channel mode
// (session.cpp). Each returns a string result for the caller to deliver.

std::string cmd_status(const Agent& agent);
std::string cmd_models(const Agent& agent, const Config& config);
std::string cmd_memory(const Agent& agent);
std::string cmd_soul(const Agent& agent, bool dev);
std::string cmd_hatch(Agent& agent);

// These mutate agent and/or config state.
std::string cmd_model(const std::string& new_model, Agent& agent,
                       Config& config, HttpClient& http);
std::string cmd_provider(const std::string& args, Agent& agent,
                          Config& config, HttpClient& http);

} // namespace ptrclaw
