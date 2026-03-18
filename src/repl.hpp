#pragma once

namespace ptrclaw {

class Agent;
class HttpClient;
struct Config;

// Run the interactive REPL loop. Returns exit code.
int run_repl(Agent& agent, Config& config, HttpClient& http,
             bool onboard_ran, bool onboard_hatch);

} // namespace ptrclaw
