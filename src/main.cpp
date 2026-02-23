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
#include <iostream>
#include <fstream>
#include <string>
#include <cstring>
#include <atomic>
#include <csignal>

static std::atomic<bool> g_shutdown{false};

static void signal_handler(int /*sig*/) {
    g_shutdown.store(true);
}

static void print_usage() {
    std::cout << "Usage: ptrclaw [options]\n"
              << "\n"
              << "Options:\n"
              << "  -m, --message MSG    Send a single message and exit\n"
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
              << "  /help                Show available commands\n"
              << "  /quit, /exit         Exit the REPL\n"
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
                       ptrclaw::HttpClient& http_client) {
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

    channel->initialize();

    // Set up event bus
    ptrclaw::EventBus bus;
    ptrclaw::SessionManager sessions(config, http_client);
    sessions.set_event_bus(&bus);

    // Wire up channel display (typing, streaming, message delivery)
    ptrclaw::StreamRelay relay(*channel, bus);
    relay.subscribe_events();

    // SessionManager subscribes last — runs after channel handler sets up
    // typing + stream state
    sessions.subscribe_events();

    uint32_t poll_count = 0;
    std::cerr << "[" << channel_name << "] Bot started. Polling for messages...\n";

    if (!channel->supports_polling()) {
        std::cerr << channel_name << " channel requires an external webhook gateway.\n"
                  << "For whatsapp: set webhook_listen (e.g. \"127.0.0.1:8080\") in\n"
                  << "~/.ptrclaw/config.json under channels.whatsapp, or via the\n"
                  << "WHATSAPP_WEBHOOK_LISTEN env var, to start the built-in webhook\n"
                  << "server. Place a reverse proxy (nginx, Caddy) in front for TLS\n"
                  << "and rate-limiting. See docs/reverse-proxy.md for details.\n";
        return 1;
    }

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

    // Initialize
    ptrclaw::http_init();
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

    // Channel mode
    if (!channel_name.empty()) {
        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);
        ptrclaw::http_set_abort_flag(&g_shutdown);

        ptrclaw::PlatformHttpClient http_client;
        int rc = run_channel(channel_name, config, http_client);

        ptrclaw::http_cleanup();
        return rc;
    }

    // Create HTTP client and provider
    ptrclaw::PlatformHttpClient http_client;
    std::unique_ptr<ptrclaw::Provider> provider;
    try {
        provider = ptrclaw::create_provider(
            config.provider,
            config.api_key_for(config.provider),
            http_client,
            config.base_url_for(config.provider));
    } catch (const std::exception& e) {
        std::cerr << "Error creating provider: " << e.what() << "\n";
        ptrclaw::http_cleanup();
        return 1;
    }

    // Create tools and agent
    auto tools = ptrclaw::create_builtin_tools();
    ptrclaw::Agent agent(std::move(provider), std::move(tools), config);

    // Single message mode
    if (!message.empty()) {
        std::string response = agent.process(message);
        std::cout << response << '\n';
        ptrclaw::http_cleanup();
        return 0;
    }

    // Interactive REPL
    std::cout << "PtrClaw AI Assistant\n"
              << "Provider: " << agent.provider_name()
              << " | Model: " << agent.model() << "\n"
              << "Type /help for commands, /quit to exit.\n\n";

    // Auto-detect unhatched agent
    if (agent.memory() && !agent.is_hatched()) {
        std::cout << "Your assistant doesn't have an identity yet. Starting hatching...\n\n";
        agent.start_hatch();
    }

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
            } else if (line == "/help") {
                std::cout << "Commands:\n"
                          << "  /status          Show current status\n"
                          << "  /model X         Switch to model X\n"
                          << "  /clear           Clear conversation history\n"
                          << "  /memory          Show memory status\n"
                          << "  /memory export   Export memories as JSON\n"
                          << "  /memory import P Import memories from JSON file\n";
                if (config.dev) {
                    std::cout << "  /soul            Show current soul/identity data\n";
                }
                std::cout << "  /hatch           Create or re-create assistant identity\n"
                          << "  /quit            Exit\n"
                          << "  /exit            Exit\n"
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

    ptrclaw::http_cleanup();
    return 0;
} catch (const std::exception& e) {
    std::cerr << "Fatal error: " << e.what() << '\n';
    return 1;
}
