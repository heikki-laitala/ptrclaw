#include "config.hpp"
#include "provider.hpp"
#include "tool.hpp"
#include "agent.hpp"
#include "http.hpp"
#include "channel.hpp"
#include "plugin.hpp"
#include "event_bus.hpp"
#include "tool_manager.hpp"
#include "session.hpp"
#include "stream_relay.hpp"
#include "onboard.hpp"
#ifdef PTRCLAW_HAS_OPENAI
#include "providers/oauth_openai.hpp"
#endif
#include "repl.hpp"
#include "util.hpp"
#ifdef PTRCLAW_HAS_EMBEDDINGS
#include "embedder.hpp"
#endif
#include <iostream>
#include <string>
#include <cstring>
#include <atomic>
#include <csignal>

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
    std::string binary_path = ptrclaw::resolve_binary_path(argv[0]);

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
#ifdef PTRCLAW_HAS_OPENAI
    ptrclaw::setup_oauth_refresh(provider.get(), config);
#endif

    ptrclaw::EventBus cli_bus;
    auto tools = ptrclaw::create_builtin_tools();
    ptrclaw::ToolManager tool_mgr(std::move(tools), config, cli_bus);

    ptrclaw::Agent agent(std::move(provider), config);
    agent.set_event_bus(&cli_bus);
    agent.set_binary_path(binary_path);

    tool_mgr.wire_memory(agent.memory());
    tool_mgr.publish_tool_specs();

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
    return ptrclaw::run_repl(agent, config, http_client, onboard_ran, onboard_hatch);
} catch (const std::exception& e) {
    std::cerr << "Fatal error: " << e.what() << '\n';
    return 1;
}
