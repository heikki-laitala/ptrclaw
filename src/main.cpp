#include "config.hpp"
#include "provider.hpp"
#include "tool.hpp"
#include "agent.hpp"
#include "memory.hpp"
#include "prompt.hpp"
#include "http.hpp"
#include "channel.hpp"
#include "plugin.hpp"
#include "event_bus.hpp"
#include "session.hpp"
#include "stream_relay.hpp"
#include "oauth.hpp"
#include "util.hpp"
#include "providers/openai.hpp"
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
              << "  /auth                OpenAI OAuth commands\n"
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

static void setup_repl_oauth_refresh(ptrclaw::Provider* provider, ptrclaw::Config& config) {
    auto* oai = dynamic_cast<ptrclaw::OpenAIProvider*>(provider);
    if (!oai) return;
    oai->set_on_token_refresh(
        [&config](const std::string& at, const std::string& rt, uint64_t ea) {
            auto& entry = config.providers["openai"];
            entry.oauth_access_token = at;
            entry.oauth_refresh_token = rt;
            entry.oauth_expires_at = ea;
            ptrclaw::persist_openai_oauth(entry);
        });
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

    // Create provider and agent (pipe, -m, REPL modes)
    std::unique_ptr<ptrclaw::Provider> provider;
    try {
        auto provider_it = config.providers.find(config.provider);
        provider = ptrclaw::create_provider(
            config.provider,
            config.api_key_for(config.provider),
            http_client,
            config.base_url_for(config.provider),
            config.prompt_caching_for(config.provider),
            provider_it != config.providers.end() ? &provider_it->second : nullptr);
    } catch (const std::exception& e) {
        std::cerr << "Error creating provider: " << e.what() << "\n";
        return 1;
    }
    setup_repl_oauth_refresh(provider.get(), config);

    auto tools = ptrclaw::create_builtin_tools();
    ptrclaw::Agent agent(std::move(provider), std::move(tools), config);
    agent.set_binary_path(binary_path);

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

    // Auto-detect unhatched agent
    if (agent.memory() && !agent.is_hatched()) {
        std::cout << "Your assistant doesn't have an identity yet. Starting hatching...\n\n";
        agent.start_hatch();
    }

    std::optional<ptrclaw::PendingOAuth> pending_oauth;

    auto finish_oauth = [&](const ptrclaw::PendingOAuth& pending,
                             const std::string& code) {
        auto r = ptrclaw::apply_oauth_result(code, pending, config, http_client);
        if (!r.success) { std::cout << r.error << "\n"; return; }
        setup_repl_oauth_refresh(r.provider.get(), config);
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
                std::cout << "Provider: " << agent.provider_name() << "\n"
                          << "Model: " << agent.model() << "\n"
                          << "History: " << agent.history_size() << " messages\n"
                          << "Estimated tokens: " << agent.estimated_tokens() << "\n";
            } else if (line == "/clear") {
                agent.clear_history();
                std::cout << "History cleared.\n";
            } else if (line.substr(0, 7) == "/model ") {
                std::string new_model = line.substr(7);
                agent.set_model(new_model);
                std::cout << "Model set to: " << new_model << "\n";
            } else if (line == "/models") {
                auto oai_it = config.providers.find("openai");
                bool use_oauth = oai_it != config.providers.end() && oai_it->second.use_oauth;
                auto infos = ptrclaw::list_providers(config, agent.provider_name(), use_oauth);

                std::cout << "Configured providers:\n";
                for (const auto& info : infos) {
                    std::cout << (info.active ? "* " : "  ") << info.name;
                    for (size_t i = info.name.size(); i < 15; ++i) std::cout << ' ';
                    std::cout << info.auth;
                    if (info.active) std::cout << "    model: " << agent.model();
                    std::cout << "\n";
                }
                std::cout << "\nSwitch: /provider <name> [model]\n";
            } else if (line.substr(0, 10) == "/provider ") {
                auto args = ptrclaw::trim(line.substr(10));
                auto space = args.find(' ');
                std::string prov_name = (space == std::string::npos) ? args : args.substr(0, space);
                std::string model_arg = (space == std::string::npos) ? "" : ptrclaw::trim(args.substr(space + 1));

                auto sr = ptrclaw::switch_provider(prov_name, model_arg, config, http_client);
                if (!sr.error.empty()) {
                    std::cout << sr.error << "\n";
                } else {
                    setup_repl_oauth_refresh(sr.provider.get(), config);
                    agent.set_provider(std::move(sr.provider));
                    if (!sr.model.empty()) agent.set_model(sr.model);
                    std::cout << "Switched to " << sr.display_name << " | Model: " << agent.model() << "\n";
                }
            } else if (line == "/memory") {
                auto* mem = agent.memory();
                if (!mem || mem->backend_name() == "none") {
                    std::cout << "Memory: disabled\n";
                } else {
                    std::cout << "Memory backend: " << mem->backend_name() << "\n"
                              << "  Core:         " << mem->count(ptrclaw::MemoryCategory::Core) << " entries\n"
                              << "  Knowledge:    " << mem->count(ptrclaw::MemoryCategory::Knowledge) << " entries\n"
                              << "  Conversation: " << mem->count(ptrclaw::MemoryCategory::Conversation) << " entries\n"
                              << "  Total:        " << mem->count(std::nullopt) << " entries\n";
                }
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
                if (!config.dev) {
                    std::cout << "Unknown command: " << line << "\n";
                } else {
                    std::string display;
                    if (agent.memory()) {
                        display = format_soul_display(agent.memory());
                    }
                    if (display.empty()) {
                        std::cout << "No soul data yet. Use /hatch to create one.\n";
                    } else {
                        std::cout << display;
                    }
                }
            } else if (line == "/hatch") {
                agent.start_hatch();
                std::cout << "Entering hatching mode...\n";

            // ── /auth commands ───────────────────────────────────
            } else if (line == "/auth status") {
                auto it = config.providers.find("openai");
                if (it == config.providers.end()) {
                    std::cout << "No OpenAI provider config found.\n";
                } else {
                    const auto& openai = it->second;
                    std::cout << "OpenAI auth: ";
                    if (openai.use_oauth) {
                        std::cout << "OAuth enabled";
                        if (!openai.oauth_access_token.empty()) {
                            std::cout << " (token present)";
                        }
                        if (openai.oauth_expires_at > 0) {
                            std::cout << "\nExpires at (epoch): "
                                      << openai.oauth_expires_at;
                        }
                    } else if (!openai.api_key.empty()) {
                        std::cout << "API key";
                    } else {
                        std::cout << "not configured";
                    }
                    std::cout << "\n";
                }
            } else if (line == "/auth openai start") {
                auto openai_it = config.providers.find("openai");
                if (openai_it == config.providers.end()) {
                    std::cout << "OpenAI provider config missing.\n";
                } else {
                    std::string state = ptrclaw::generate_id();
                    std::string verifier = ptrclaw::make_code_verifier();
                    std::string challenge = ptrclaw::make_code_challenge_s256(verifier);
                    std::string client_id = openai_it->second.oauth_client_id.empty()
                        ? ptrclaw::kDefaultOAuthClientId
                        : openai_it->second.oauth_client_id;

                    ptrclaw::PendingOAuth pending;
                    pending.provider = "openai";
                    pending.state = state;
                    pending.code_verifier = verifier;
                    pending.redirect_uri = ptrclaw::kDefaultRedirectUri;
                    pending.created_at = ptrclaw::epoch_seconds();
                    pending_oauth = std::move(pending);

                    std::string url = ptrclaw::build_authorize_url(
                        client_id, ptrclaw::kDefaultRedirectUri, challenge, state);

                    std::cout << "Open this URL to authorize OpenAI:\n" << url
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
            } else if (line == "/auth") {
                std::cout << "Auth commands:\n"
                          << "  /auth status                Show OpenAI auth status\n"
                          << "  /auth openai start          Start OAuth flow\n"
                          << "  /auth openai finish <url>   Complete OAuth with callback URL or code\n";
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
                          << "  /auth            OpenAI OAuth commands\n"
                          << "  /auth status     Show OpenAI auth status\n"
                          << "  /auth openai start   Start OAuth flow\n";
                if (config.dev) {
                    std::cout << "  /soul            Show current soul/identity data\n";
                }
                std::cout << "  /hatch           Create or re-create assistant identity\n"
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
