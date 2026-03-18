#include "repl.hpp"
#include "agent.hpp"
#include "commands.hpp"
#include "config.hpp"
#include "http.hpp"
#include "memory.hpp"
#include "oauth.hpp"
#include "onboard.hpp"
#include "plugin.hpp"
#include "provider.hpp"
#include "util.hpp"
#ifdef PTRCLAW_HAS_OPENAI
#include "providers/oauth_openai.hpp"
#endif

#include <iostream>
#include <optional>
#include <string>

namespace ptrclaw {

int run_repl(Agent& agent, Config& config, HttpClient& http,
             bool onboard_ran, bool onboard_hatch) {
    std::cout << "PtrClaw AI Assistant\n"
              << "Provider: " << agent.provider_name()
              << " | Model: " << agent.model() << "\n"
              << "Type /help for commands, /exit to exit.\n\n";

    // Auto-detect unhatched agent (skip if onboarding ran and user declined)
    if (agent.memory() && !agent.is_hatched()) {
        bool do_hatch = false;
        if (onboard_ran) {
            if (onboard_hatch) {
                std::cout << "Starting hatching...\n\n";
                do_hatch = true;
            }
        } else {
            std::cout << "Your assistant doesn't have an identity yet. Starting hatching...\n\n";
            do_hatch = true;
        }
        if (do_hatch) {
            agent.start_hatch();
            std::string response = agent.process("Begin the hatching interview.");
            std::cout << response << "\n\n";
        }
    }

#ifdef PTRCLAW_HAS_OPENAI
    std::optional<PendingOAuth> pending_oauth;

    auto finish_oauth = [&](const PendingOAuth& pending,
                             const std::string& code) {
        auto r = apply_oauth_result(code, pending, config, http);
        if (!r.success) { std::cout << r.error << "\n"; return; }
        setup_oauth_refresh(r.provider.get(), config);
        agent.set_provider(std::move(r.provider));
        agent.set_model(kDefaultOAuthModel);
        pending_oauth.reset();
        std::cout << "OpenAI OAuth connected. Model switched to "
                  << kDefaultOAuthModel << "."
                  << (r.persisted
                      ? " Saved to ~/.ptrclaw/config.json\n"
                      : " (warning: could not persist to config file)\n");
    };
#endif

    std::string line;
    while (true) {
        std::cout << "ptrclaw> " << std::flush;

        if (!std::getline(std::cin, line)) {
            // EOF (Ctrl+D)
            std::cout << "\n";
            break;
        }

        // Skip empty lines
        if (line.empty()) continue;

#ifdef PTRCLAW_HAS_OPENAI
        // Raw OAuth paste detection (when auth flow is pending)
        if (pending_oauth && line[0] != '/') {
            std::string raw = trim(line);
            bool looks_like_oauth =
                (!raw.empty() &&
                 (raw.find("code=") != std::string::npos ||
                  raw.find("auth/callback") != std::string::npos ||
                  raw.find("localhost:1455") != std::string::npos));
            if (looks_like_oauth) {
                auto parsed = parse_oauth_input(raw);
                if (parsed.code.empty()) {
                    std::cout << "Missing code. Paste callback URL or auth code.\n";
                } else if (!parsed.state.empty() && parsed.state != pending_oauth->state) {
                    std::cout << "State mismatch. Please restart with /auth openai start\n";
                } else {
                    finish_oauth(*pending_oauth, parsed.code);
                }
                continue;
            }
        }
#endif

        // Handle slash commands
        if (line[0] == '/') {
            if (line == "/quit" || line == "/exit") {
                break;
            } else if (line == "/status") {
                std::cout << cmd_status(agent);
            } else if (line == "/clear") {
                agent.clear_history();
                std::cout << "History cleared.\n";
            } else if (line.substr(0, 7) == "/model ") {
                std::cout << cmd_model(line.substr(7), agent,
                                        config, http) << "\n";
            } else if (line == "/models") {
                std::cout << cmd_models(agent, config) << "\n";
            } else if (line.substr(0, 10) == "/provider ") {
                std::cout << cmd_provider(line.substr(10), agent,
                                           config, http) << "\n";
            } else if (line == "/memory") {
                std::cout << cmd_memory(agent) << "\n";
            } else if (line == "/memory export") {
                std::cout << cmd_memory_export(agent) << "\n";
            } else if (line.substr(0, 15) == "/memory import ") {
                std::cout << cmd_memory_import(agent, line.substr(15)) << "\n";
            } else if (line == "/soul") {
                std::cout << cmd_soul(agent, config.dev) << "\n";
            } else if (line == "/onboard") {
                bool hatch_req = false;
                if (run_onboard(config, http, hatch_req)) {
                    auto sr = switch_provider(
                        config.provider, config.model, config.model, config, http);
                    if (!sr.error.empty()) {
                        std::cout << sr.error << "\n";
                    } else {
#ifdef PTRCLAW_HAS_OPENAI
                        setup_oauth_refresh(sr.provider.get(), config);
#endif
                        agent.set_provider(std::move(sr.provider));
                        if (!sr.model.empty()) agent.set_model(sr.model);
                        std::cout << "Provider: " << agent.provider_name()
                                  << " | Model: " << agent.model() << "\n";
                        if (hatch_req) {
                            agent.start_hatch();
                            std::string r = agent.process("Begin the hatching interview.");
                            std::cout << r << "\n";
                        }
                    }
                }
            } else if (line == "/skill" || line.substr(0, 7) == "/skill ") {
                std::string args = (line.size() > 7) ? line.substr(7) : "";
                std::cout << cmd_skill(args, agent) << "\n";
            } else if (line == "/hatch") {
                std::cout << cmd_hatch(agent) << "\n";

            // ── /auth commands ───────────────────────────────────
#ifdef PTRCLAW_HAS_OPENAI
            } else if (line == "/auth openai start") {
                auto openai_it = config.providers.find("openai");
                if (openai_it == config.providers.end()) {
                    std::cout << "OpenAI provider config missing.\n";
                } else {
                    auto flow = start_oauth_flow(openai_it->second);
                    pending_oauth = std::move(flow.pending);

                    std::cout << "Open this URL to authorize OpenAI:\n" << flow.authorize_url
                              << "\n\nThen paste the full callback URL with:\n"
                              << "/auth openai finish <callback_url>\n"
                              << "(or paste just the code)\n";
                }
            } else if (line.rfind("/auth openai finish ", 0) == 0) {
                if (!pending_oauth || pending_oauth->provider != "openai") {
                    std::cout << "No pending OpenAI auth flow. Start with: /auth openai start\n";
                } else {
                    std::string input = line.substr(
                        line.find("finish") + 7);
                    auto parsed = parse_oauth_input(input);

                    if (parsed.code.empty()) {
                        std::cout << "Missing code. Paste callback URL or auth code.\n";
                    } else if (!parsed.state.empty() && parsed.state != pending_oauth->state) {
                        std::cout << "State mismatch. Please restart with /auth openai start\n";
                    } else {
                        finish_oauth(*pending_oauth, parsed.code);
                    }
                }
#endif
            } else if (line.rfind("/auth ", 0) == 0) {
                // /auth <provider> — interactive credential setup
                std::string prov = trim(line.substr(6));
                auto& reg = PluginRegistry::instance();
                auto all = reg.provider_names();
                bool known = false;
                for (const auto& n : all) {
                    if (n == prov) { known = true; break; }
                }
                if (is_hidden_provider(prov)) known = false;
                if (!known) {
                    std::cout << "Unknown provider: " << prov << "\n";
                } else if (prov == "ollama") {
                    auto it = config.providers.find("ollama");
                    std::string current = (it != config.providers.end())
                        ? it->second.base_url : "http://localhost:11434";
                    std::cout << "Base URL [" << current << "]: " << std::flush;
                    std::string url;
                    std::getline(std::cin, url);
                    url = trim(url);
                    if (url.empty()) url = current;
                    config.providers["ollama"].base_url = url;
                    modify_config_json([&](nlohmann::json& j) {
                        j["providers"]["ollama"]["base_url"] = url;
                    });
                    std::cout << "Saved.\n";
#ifdef PTRCLAW_HAS_OPENAI
                } else if (prov == "openai") {
                    std::cout << "Authentication method:\n"
                              << "  1. API key\n"
                              << "  2. OAuth login (ChatGPT subscription)\n"
                              << "> " << std::flush;
                    std::string choice_str;
                    std::getline(std::cin, choice_str);
                    choice_str = trim(choice_str);
                    if (choice_str == "2") {
                        // Inline OAuth flow
                        auto flow = start_oauth_flow(config.providers["openai"]);
                        std::cout << "\nOpen this URL to authorize:\n" << flow.authorize_url
                                  << "\n\nPaste the callback URL or code: " << std::flush;
                        std::string input;
                        std::getline(std::cin, input);
                        input = trim(input);
                        if (input.empty()) {
                            std::cout << "Skipped.\n";
                        } else {
                            auto parsed = parse_oauth_input(input);
                            if (parsed.code.empty()) {
                                std::cout << "Could not extract auth code.\n";
                            } else if (!parsed.state.empty() && parsed.state != flow.pending.state) {
                                std::cout << "State mismatch. Please try again.\n";
                            } else {
                                finish_oauth(flow.pending, parsed.code);
                            }
                        }
                    } else {
                        std::cout << "Enter your " << provider_label("openai")
                                  << " API key: " << std::flush;
                        std::string api_key;
                        std::getline(std::cin, api_key);
                        api_key = trim(api_key);
                        if (api_key.empty()) {
                            std::cout << "No API key provided.\n";
                        } else {
                            config.providers["openai"].api_key = api_key;
                            persist_provider_key("openai", api_key);
                            std::cout << "Saved.\n";
                        }
                    }
#endif
                } else {
                    // Other providers: API key prompt
                    std::cout << "Enter your " << provider_label(prov)
                              << " API key: " << std::flush;
                    std::string api_key;
                    std::getline(std::cin, api_key);
                    api_key = trim(api_key);
                    if (api_key.empty()) {
                        std::cout << "No API key provided.\n";
                    } else {
                        config.providers[prov].api_key = api_key;
                        persist_provider_key(prov, api_key);
                        std::cout << "Saved.\n";
                    }
                }
                // Re-create active provider if credentials changed
                if (prov == config.provider) {
                    auto sr = switch_provider(
                        config.provider, config.model, config.model, config, http);
                    if (!sr.error.empty()) {
                        std::cout << sr.error << "\n";
                    } else {
#ifdef PTRCLAW_HAS_OPENAI
                        setup_oauth_refresh(sr.provider.get(), config);
#endif
                        agent.set_provider(std::move(sr.provider));
                        if (!sr.model.empty()) agent.set_model(sr.model);
                    }
                }
            } else if (line == "/auth") {
                std::cout << cmd_auth_status(config) << "\n";
            } else if (line == "/help") {
                std::cout << cmd_help(config.dev);
            } else {
                std::cout << "Unknown command: " << line << "\n";
            }
            continue;
        }

        // Process user message
        std::string response = agent.process(line);
        std::cout << "\n" << response << "\n\n";
    }

    return 0;
}

} // namespace ptrclaw
