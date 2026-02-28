#include "commands.hpp"
#include "config.hpp"
#include "provider.hpp"
#include "tool.hpp"
#include "agent.hpp"
#include "memory.hpp"
#include "http.hpp"
#include "channel.hpp"
#include "plugin.hpp"
#include "event_bus.hpp"
#include "session.hpp"
#include "stream_relay.hpp"
#include "oauth.hpp"
#include "onboard.hpp"
#include "util.hpp"
#ifdef PTRCLAW_HAS_EMBEDDINGS
#include "embedder.hpp"
#endif
#include <iostream>
#include <fstream>
#include <string>
#include <cstring>
#include <atomic>
#include <csignal>
#include <filesystem>
#include <sstream>

namespace {

struct HttpGuard {
    HttpGuard() { ptrclaw::http_init(); }
    ~HttpGuard() { ptrclaw::http_cleanup(); }
    HttpGuard(const HttpGuard&) = delete;
    HttpGuard& operator=(const HttpGuard&) = delete;
};

} // namespace

static std::atomic<bool> g_shutdown{false};

static void signal_handler(int /*sig*/) {
    g_shutdown.store(true);
}

static std::string resolve_binary_path(const char* argv0) {
    std::string path(argv0);

    // Absolute or relative path — resolve via filesystem
    if (path.find('/') != std::string::npos) {
        std::error_code ec;
        auto canonical = std::filesystem::canonical(path, ec);
        if (!ec) return canonical.string();
        return path;
    }

    // Bare name — search PATH
    const char* path_env = std::getenv("PATH");
    if (!path_env) return path;

    std::istringstream dirs(path_env);
    std::string dir;
    while (std::getline(dirs, dir, ':')) {
        auto candidate = std::filesystem::path(dir) / path;
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec)) {
            auto canonical = std::filesystem::canonical(candidate, ec);
            if (!ec) return canonical.string();
        }
    }
    return path;
}

static void print_usage() {
    std::cout << "Usage: ptrclaw [options]\n"
              << "\n"
              << "Options:\n"
              << "  -m, --message MSG    Send a single message and exit\n"
              << "  --notify CHAN:TARGET  After -m, send response via channel (e.g. telegram:123456)\n"
              << "  --channel NAME       Run as a channel bot (telegram, whatsapp)\n"
              << "  --provider NAME      Use specific provider (anthropic, openai, ollama, openrouter)\n"
              << "  --model NAME         Use specific model\n"
              << "  --dev                Enable developer-only commands (e.g. /soul)\n"
              << "  -h, --help           Show this help\n"
              << "\n"
              << "Interactive commands:\n"
              << "  /status              Show current model, provider, history info\n"
              << "  /model NAME          Switch model\n"
              << "  /clear               Clear conversation history\n"
              << "  /auth                Show auth status / set credentials\n"
              << "  /help                Show available commands\n"
              << "  /exit, /quit         Exit the REPL\n"
              << "\n"
              << "Environment variables:\n"
              << "  ANTHROPIC_API_KEY    API key for Anthropic\n"
              << "  OPENAI_API_KEY       API key for OpenAI\n"
              << "  OPENROUTER_API_KEY   API key for OpenRouter\n"
              << "  OLLAMA_BASE_URL      Base URL for Ollama (default: http://localhost:11434)\n"
              << "  TELEGRAM_BOT_TOKEN   Telegram bot token (for --channel telegram)\n"
              << "  WHATSAPP_ACCESS_TOKEN  WhatsApp access token (for --channel whatsapp)\n"
              << "  WHATSAPP_PHONE_ID    WhatsApp phone number ID\n"
              << "  WHATSAPP_VERIFY_TOKEN  WhatsApp webhook verify token\n";
}

static int run_channel(const std::string& channel_name,
                       ptrclaw::Config& config,
                       ptrclaw::HttpClient& http_client,
                       const std::string& binary_path) {
    // Create channel via plugin registry
    std::unique_ptr<ptrclaw::Channel> channel;
    try {
        channel = ptrclaw::PluginRegistry::instance().create_channel(
            channel_name, config, http_client);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    if (!channel->health_check()) {
        std::cerr << "Error: " << channel_name << " health check failed.\n";
        return 1;
    }

    if (!channel->supports_polling()) {
        std::cerr << channel_name << " channel requires an external webhook gateway.\n"
                  << "For whatsapp: set webhook_listen (e.g. \"127.0.0.1:8080\") in\n"
                  << "~/.ptrclaw/config.json under channels.whatsapp, or via the\n"
                  << "WHATSAPP_WEBHOOK_LISTEN env var, to start the built-in webhook\n"
                  << "server. Place a reverse proxy (nginx, Caddy) in front for TLS\n"
                  << "and rate-limiting. See docs/reverse-proxy.md for details.\n";
        return 1;
    }

    channel->initialize();

    // Set up event bus
    ptrclaw::EventBus bus;
    ptrclaw::SessionManager sessions(config, http_client);
    sessions.set_binary_path(binary_path);
    sessions.set_event_bus(&bus);

#ifdef PTRCLAW_HAS_EMBEDDINGS
    auto channel_embedder = ptrclaw::create_embedder(config, http_client);
    if (channel_embedder) {
        sessions.set_embedder(channel_embedder.get());
    }
#endif

    // Wire up channel display (typing, streaming, message delivery)
    ptrclaw::StreamRelay relay(*channel, bus);
    relay.subscribe_events();

    // SessionManager subscribes last — runs after channel handler sets up
    // typing + stream state
    sessions.subscribe_events();

    uint32_t poll_count = 0;
    std::cerr << "[" << channel_name << "] Bot started. Polling for messages...\n";

    while (!g_shutdown.load()) {
        auto messages = channel->poll_updates();
        for (auto& msg : messages) {
            ptrclaw::MessageReceivedEvent ev;
            ev.session_id = msg.sender;
            ev.message = std::move(msg);
            bus.publish(ev);
        }

        // Periodic session eviction
        if (++poll_count % 100 == 0) {
            sessions.evict_idle(3600);
        }
    }

    std::cerr << "[" << channel_name << "] Shutting down.\n";
    return 0;
}


int main(int argc, char* argv[]) try {
    // Parse arguments
    std::string message;
    std::string provider_name;
    std::string model_name;
    std::string channel_name;
    std::string notify_spec;
    bool dev_mode = false;

    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
            print_usage();
            return 0;
        } else if ((std::strcmp(argv[i], "-m") == 0 || std::strcmp(argv[i], "--message") == 0) && i + 1 < argc) {
            message = argv[++i];
        } else if (std::strcmp(argv[i], "--provider") == 0 && i + 1 < argc) {
            provider_name = argv[++i];
        } else if (std::strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
            model_name = argv[++i];
        } else if (std::strcmp(argv[i], "--notify") == 0 && i + 1 < argc) {
            notify_spec = argv[++i];
        } else if (std::strcmp(argv[i], "--channel") == 0 && i + 1 < argc) {
            channel_name = argv[++i];
        } else if (std::strcmp(argv[i], "--dev") == 0) {
            dev_mode = true;
        } else {
            std::cerr << "Unknown option: " << argv[i] << "\n";
            print_usage();
            return 1;
        }
    }

    // Validate --notify requires -m
    if (!notify_spec.empty() && message.empty()) {
        std::cerr << "Error: --notify requires -m (single message mode)\n";
        return 1;
    }

    // Validate --notify format
    if (!notify_spec.empty() && notify_spec.find(':') == std::string::npos) {
        std::cerr << "Error: --notify must be CHANNEL:TARGET (e.g. telegram:123456)\n";
        return 1;
    }

    // Resolve binary path for cron scheduling
    std::string binary_path = resolve_binary_path(argv[0]);

    // Initialize
    HttpGuard http_guard;
    auto config = ptrclaw::Config::load();

    // Override config with CLI args
    if (dev_mode) {
        config.dev = true;
    }
    if (!provider_name.empty()) {
        config.provider = provider_name;
    }
    if (!model_name.empty()) {
        config.model = model_name;
    }

    ptrclaw::PlatformHttpClient http_client;

    // Channel mode (except pipe, which needs an agent below)
    if (!channel_name.empty()) {
#ifdef PTRCLAW_HAS_PIPE
        if (channel_name != "pipe")
#endif
        {
            std::signal(SIGINT, signal_handler);
            std::signal(SIGTERM, signal_handler);
            ptrclaw::http_set_abort_flag(&g_shutdown);
            return run_channel(channel_name, config, http_client, binary_path);
        }
    }

    // Auto-onboard if no provider has credentials (first run, REPL only)
    bool onboard_ran = false;
    bool onboard_hatch = false;
    if (message.empty() && channel_name.empty() && ptrclaw::needs_onboard(config)) {
        onboard_ran = ptrclaw::run_onboard(config, http_client, onboard_hatch);
    }

    // Create provider and agent (pipe, -m, REPL modes)
    std::unique_ptr<ptrclaw::Provider> provider;
    try {
        auto sr = ptrclaw::switch_provider(
            config.provider, config.model, config.model, config, http_client);
        if (!sr.error.empty()) throw std::runtime_error(sr.error);
        provider = std::move(sr.provider);
    } catch (const std::exception& e) {
        std::cerr << "Error creating provider: " << e.what() << "\n";
        return 1;
    }
    ptrclaw::setup_oauth_refresh(provider.get(), config);

    auto tools = ptrclaw::create_builtin_tools();
    ptrclaw::Agent agent(std::move(provider), std::move(tools), config);
    agent.set_binary_path(binary_path);

#ifdef PTRCLAW_HAS_EMBEDDINGS
    auto embedder = ptrclaw::create_embedder(config, http_client);
    if (embedder) {
        agent.set_embedder(embedder.get());
    }
#endif

#ifdef PTRCLAW_HAS_PIPE
    // Pipe mode: JSONL on stdin/stdout for scripted multi-turn conversations
    if (channel_name == "pipe") {
        std::string line;
        while (std::getline(std::cin, line)) {
            auto j = nlohmann::json::parse(line);
            std::string response = agent.process(j.value("content", ""));
            nlohmann::json out = {{"content", response}};
            std::cout << out.dump() << "\n" << std::flush;
        }
        return 0;
    }
#endif

    // Single message mode
    if (!message.empty()) {
        std::string response = agent.process(message);
        std::cout << response << '\n';

        // Send notification if --notify was specified
        if (!notify_spec.empty()) {
            auto colon = notify_spec.find(':');
            std::string chan_name = notify_spec.substr(0, colon);
            std::string target = notify_spec.substr(colon + 1);
            try {
                auto channel = ptrclaw::PluginRegistry::instance().create_channel(
                    chan_name, config, http_client);
                channel->send_message(target, response);
            } catch (const std::exception& e) {
                std::cerr << "Notification failed: " << e.what() << "\n";
            }
        }

        return 0;
    }

    // Interactive REPL
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

    std::optional<ptrclaw::PendingOAuth> pending_oauth;

    auto finish_oauth = [&](const ptrclaw::PendingOAuth& pending,
                             const std::string& code) {
        auto r = ptrclaw::apply_oauth_result(code, pending, config, http_client);
        if (!r.success) { std::cout << r.error << "\n"; return; }
        ptrclaw::setup_oauth_refresh(r.provider.get(), config);
        agent.set_provider(std::move(r.provider));
        agent.set_model(ptrclaw::kDefaultOAuthModel);
        pending_oauth.reset();
        std::cout << "OpenAI OAuth connected. Model switched to "
                  << ptrclaw::kDefaultOAuthModel << "."
                  << (r.persisted
                      ? " Saved to ~/.ptrclaw/config.json\n"
                      : " (warning: could not persist to config file)\n");
    };

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

        // Raw OAuth paste detection (when auth flow is pending)
        if (pending_oauth && line[0] != '/') {
            std::string raw = ptrclaw::trim(line);
            bool looks_like_oauth =
                (!raw.empty() &&
                 (raw.find("code=") != std::string::npos ||
                  raw.find("auth/callback") != std::string::npos ||
                  raw.find("localhost:1455") != std::string::npos));
            if (looks_like_oauth) {
                auto parsed = ptrclaw::parse_oauth_input(raw);
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

        // Handle slash commands
        if (line[0] == '/') {
            if (line == "/quit" || line == "/exit") {
                break;
            } else if (line == "/status") {
                std::cout << ptrclaw::cmd_status(agent);
            } else if (line == "/clear") {
                agent.clear_history();
                std::cout << "History cleared.\n";
            } else if (line.substr(0, 7) == "/model ") {
                std::cout << ptrclaw::cmd_model(line.substr(7), agent,
                                                config, http_client) << "\n";
            } else if (line == "/models") {
                std::cout << ptrclaw::cmd_models(agent, config) << "\n";
            } else if (line.substr(0, 10) == "/provider ") {
                std::cout << ptrclaw::cmd_provider(line.substr(10), agent,
                                                   config, http_client) << "\n";
            } else if (line == "/memory") {
                std::cout << ptrclaw::cmd_memory(agent) << "\n";
            } else if (line == "/memory export") {
                auto* mem = agent.memory();
                if (!mem || mem->backend_name() == "none") {
                    std::cout << "Memory: disabled\n";
                } else {
                    std::cout << mem->snapshot_export() << "\n";
                }
            } else if (line.substr(0, 15) == "/memory import ") {
                auto* mem = agent.memory();
                if (!mem || mem->backend_name() == "none") {
                    std::cout << "Memory: disabled\n";
                } else {
                    std::string path = line.substr(15);
                    std::ifstream file(path);
                    if (!file.is_open()) {
                        std::cout << "Error: cannot open " << path << "\n";
                    } else {
                        std::string content((std::istreambuf_iterator<char>(file)),
                                             std::istreambuf_iterator<char>());
                        uint32_t n = mem->snapshot_import(content);
                        std::cout << "Imported " << n << " entries.\n";
                    }
                }
            } else if (line == "/soul") {
                std::cout << ptrclaw::cmd_soul(agent, config.dev) << "\n";
            } else if (line == "/onboard") {
                bool hatch_req = false;
                if (ptrclaw::run_onboard(config, http_client, hatch_req)) {
                    auto sr = ptrclaw::switch_provider(
                        config.provider, config.model, config.model, config, http_client);
                    if (!sr.error.empty()) {
                        std::cout << sr.error << "\n";
                    } else {
                        ptrclaw::setup_oauth_refresh(sr.provider.get(), config);
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
            } else if (line == "/hatch") {
                std::cout << ptrclaw::cmd_hatch(agent) << "\n";

            // ── /auth commands ───────────────────────────────────
            } else if (line == "/auth openai start") {
                auto openai_it = config.providers.find("openai");
                if (openai_it == config.providers.end()) {
                    std::cout << "OpenAI provider config missing.\n";
                } else {
                    auto flow = ptrclaw::start_oauth_flow(openai_it->second);
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
                    auto parsed = ptrclaw::parse_oauth_input(input);

                    if (parsed.code.empty()) {
                        std::cout << "Missing code. Paste callback URL or auth code.\n";
                    } else if (!parsed.state.empty() && parsed.state != pending_oauth->state) {
                        std::cout << "State mismatch. Please restart with /auth openai start\n";
                    } else {
                        finish_oauth(*pending_oauth, parsed.code);
                    }
                }
            } else if (line.rfind("/auth ", 0) == 0) {
                // /auth <provider> — interactive credential setup
                std::string prov = ptrclaw::trim(line.substr(6));
                auto& reg = ptrclaw::PluginRegistry::instance();
                auto all = reg.provider_names();
                bool known = false;
                for (const auto& n : all) {
                    if (n == prov) { known = true; break; }
                }
                if (ptrclaw::is_hidden_provider(prov)) known = false;
                if (!known) {
                    std::cout << "Unknown provider: " << prov << "\n";
                } else if (prov == "ollama") {
                    auto it = config.providers.find("ollama");
                    std::string current = (it != config.providers.end())
                        ? it->second.base_url : "http://localhost:11434";
                    std::cout << "Base URL [" << current << "]: " << std::flush;
                    std::string url;
                    std::getline(std::cin, url);
                    url = ptrclaw::trim(url);
                    if (url.empty()) url = current;
                    config.providers["ollama"].base_url = url;
                    ptrclaw::modify_config_json([&](nlohmann::json& j) {
                        j["providers"]["ollama"]["base_url"] = url;
                    });
                    std::cout << "Saved.\n";
                } else if (prov == "openai") {
                    std::cout << "Authentication method:\n"
                              << "  1. API key\n"
                              << "  2. OAuth login (ChatGPT subscription)\n"
                              << "> " << std::flush;
                    std::string choice_str;
                    std::getline(std::cin, choice_str);
                    choice_str = ptrclaw::trim(choice_str);
                    if (choice_str == "2") {
                        // Inline OAuth flow
                        auto flow = ptrclaw::start_oauth_flow(config.providers["openai"]);
                        std::cout << "\nOpen this URL to authorize:\n" << flow.authorize_url
                                  << "\n\nPaste the callback URL or code: " << std::flush;
                        std::string input;
                        std::getline(std::cin, input);
                        input = ptrclaw::trim(input);
                        if (input.empty()) {
                            std::cout << "Skipped.\n";
                        } else {
                            auto parsed = ptrclaw::parse_oauth_input(input);
                            if (parsed.code.empty()) {
                                std::cout << "Could not extract auth code.\n";
                            } else if (!parsed.state.empty() && parsed.state != flow.pending.state) {
                                std::cout << "State mismatch. Please try again.\n";
                            } else {
                                finish_oauth(flow.pending, parsed.code);
                            }
                        }
                    } else {
                        std::cout << "Enter your " << ptrclaw::provider_label("openai")
                                  << " API key: " << std::flush;
                        std::string api_key;
                        std::getline(std::cin, api_key);
                        api_key = ptrclaw::trim(api_key);
                        if (api_key.empty()) {
                            std::cout << "No API key provided.\n";
                        } else {
                            config.providers["openai"].api_key = api_key;
                            ptrclaw::persist_provider_key("openai", api_key);
                            std::cout << "Saved.\n";
                        }
                    }
                } else {
                    // Other providers: API key prompt
                    std::cout << "Enter your " << ptrclaw::provider_label(prov)
                              << " API key: " << std::flush;
                    std::string api_key;
                    std::getline(std::cin, api_key);
                    api_key = ptrclaw::trim(api_key);
                    if (api_key.empty()) {
                        std::cout << "No API key provided.\n";
                    } else {
                        config.providers[prov].api_key = api_key;
                        ptrclaw::persist_provider_key(prov, api_key);
                        std::cout << "Saved.\n";
                    }
                }
                // Re-create active provider if credentials changed
                if (prov == config.provider) {
                    auto sr = ptrclaw::switch_provider(
                        config.provider, config.model, config.model, config, http_client);
                    if (!sr.error.empty()) {
                        std::cout << sr.error << "\n";
                    } else {
                        ptrclaw::setup_oauth_refresh(sr.provider.get(), config);
                        agent.set_provider(std::move(sr.provider));
                        if (!sr.model.empty()) agent.set_model(sr.model);
                    }
                }
            } else if (line == "/auth") {
                std::cout << ptrclaw::format_auth_status(config)
                          << "\nSet credentials: /auth <provider>\n";
            } else if (line == "/help") {
                std::cout << "Commands:\n"
                          << "  /status          Show current status\n"
                          << "  /model X         Switch to model X\n"
                          << "  /models          List configured providers\n"
                          << "  /provider X [M]  Switch to provider X, optional model M\n"
                          << "  /clear           Clear conversation history\n"
                          << "  /memory          Show memory status\n"
                          << "  /memory export   Export memories as JSON\n"
                          << "  /memory import P Import memories from JSON file\n"
                          << "  /auth            Show auth status for all providers\n"
                          << "  /auth <provider> Set credentials for a provider\n";
                if (config.dev) {
                    std::cout << "  /soul            Show current soul/identity data\n";
                }
                std::cout << "  /hatch           Create or re-create assistant identity\n"
                          << "  /onboard         Run setup wizard\n"
                          << "  /exit, /quit     Exit\n"
                          << "  /help            Show this help\n";
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
} catch (const std::exception& e) {
    std::cerr << "Fatal error: " << e.what() << '\n';
    return 1;
}
