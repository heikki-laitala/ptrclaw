#include "config.hpp"
#include "provider.hpp"
#include "tool.hpp"
#include "agent.hpp"
#include "http.hpp"
#include "channel.hpp"
#include "channels/telegram.hpp"
#include "channels/whatsapp.hpp"
#include "session.hpp"
#include <iostream>
#include <string>
#include <cstring>
#include <thread>
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

static int run_telegram(ptrclaw::Config& config, ptrclaw::CurlHttpClient& http_client) {
    if (!config.channels.telegram || config.channels.telegram->bot_token.empty()) {
        std::cerr << "Error: Telegram bot_token not configured.\n"
                  << "Set TELEGRAM_BOT_TOKEN or add channels.telegram.bot_token to config.\n";
        return 1;
    }

    auto& tc = *config.channels.telegram;
    ptrclaw::TelegramConfig tg_cfg;
    tg_cfg.bot_token = tc.bot_token;
    tg_cfg.allow_from = tc.allow_from;
    tg_cfg.reply_in_private = tc.reply_in_private;
    tg_cfg.proxy = tc.proxy;

    ptrclaw::TelegramChannel telegram(tg_cfg, http_client);

    if (!telegram.health_check()) {
        std::cerr << "Error: Telegram bot token is invalid (health check failed).\n";
        return 1;
    }

    telegram.set_my_commands();
    telegram.drop_pending_updates();

    ptrclaw::SessionManager sessions(config, http_client);
    uint32_t poll_count = 0;

    std::cerr << "[telegram] Bot started. Polling for messages...\n";

    while (!g_shutdown.load()) {
        auto messages = telegram.poll_updates();

        for (const auto& msg : messages) {
            std::string session_id = msg.sender;
            auto& agent = sessions.get_session(session_id);

            // Handle /start command
            if (msg.content == "/start") {
                std::string greeting = "Hello";
                if (msg.first_name) greeting += " " + *msg.first_name;
                greeting += "! I'm PtrClaw, an AI assistant. How can I help you?";
                if (msg.reply_target) {
                    telegram.send_message(*msg.reply_target, greeting);
                }
                continue;
            }

            // Handle /new command
            if (msg.content == "/new") {
                agent.clear_history();
                if (msg.reply_target) {
                    telegram.send_message(*msg.reply_target, "Conversation cleared. What would you like to discuss?");
                }
                continue;
            }

            std::string response = agent.process(msg.content);
            if (msg.reply_target) {
                telegram.send_message(*msg.reply_target, response);
            }
        }

        // Periodic session eviction
        if (++poll_count % 100 == 0) {
            sessions.evict_idle(3600);
        }
    }

    std::cerr << "[telegram] Shutting down.\n";
    return 0;
}

int main(int argc, char* argv[]) try {
    // Parse arguments
    std::string message;
    std::string provider_name;
    std::string model_name;
    std::string channel_name;

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
    if (!provider_name.empty()) {
        config.default_provider = provider_name;
    }
    if (!model_name.empty()) {
        config.default_model = model_name;
    }

    // Channel mode
    if (!channel_name.empty()) {
        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);

        ptrclaw::CurlHttpClient http_client;
        int rc = 1;

        if (channel_name == "telegram") {
            rc = run_telegram(config, http_client);
        } else if (channel_name == "whatsapp") {
            std::cerr << "WhatsApp channel requires an external webhook gateway.\n"
                      << "Use the WhatsAppChannel API programmatically with your HTTP server.\n";
            rc = 1;
        } else {
            std::cerr << "Unknown channel: " << channel_name << "\n"
                      << "Available channels: telegram, whatsapp\n";
        }

        ptrclaw::http_cleanup();
        return rc;
    }

    // Create HTTP client and provider
    ptrclaw::CurlHttpClient http_client;
    std::unique_ptr<ptrclaw::Provider> provider;
    try {
        provider = ptrclaw::create_provider(
            config.default_provider,
            config.api_key_for(config.default_provider),
            http_client,
            config.ollama_base_url);
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
            } else if (line == "/help") {
                std::cout << "Commands:\n"
                          << "  /status   Show current status\n"
                          << "  /model X  Switch to model X\n"
                          << "  /clear    Clear conversation history\n"
                          << "  /quit     Exit\n"
                          << "  /exit     Exit\n"
                          << "  /help     Show this help\n";
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
