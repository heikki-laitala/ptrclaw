#include "config.hpp"
#include "provider.hpp"
#include "tool.hpp"
#include "agent.hpp"
#include "http.hpp"
#include "channel.hpp"
#include "plugin.hpp"
#include "event_bus.hpp"
#include "event.hpp"
#include "session.hpp"
#include <iostream>
#include <string>
#include <cstring>
#include <thread>
#include <atomic>
#include <csignal>
#include <chrono>
#include <unordered_map>

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

static int run_channel(const std::string& channel_name,
                       ptrclaw::Config& config,
                       ptrclaw::CurlHttpClient& http_client) {
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

    // Streaming display state
    struct StreamState {
        std::string chat_id;
        int64_t message_id = 0;
        std::string accumulated;
        std::chrono::steady_clock::time_point last_edit;
        bool delivered = false;
    };
    std::unordered_map<std::string, StreamState> stream_states;

    // MessageReady → send via channel (skip if already delivered via streaming)
    ptrclaw::subscribe<ptrclaw::MessageReadyEvent>(bus,
        [&channel, &stream_states](const ptrclaw::MessageReadyEvent& ev) {
            auto it = stream_states.find(ev.session_id);
            if (it != stream_states.end()) {
                bool delivered = it->second.delivered;
                stream_states.erase(it);
                if (delivered) return;
            }
            if (!ev.reply_target.empty()) {
                channel->send_message(ev.reply_target, ev.content);
            }
        });

    // MessageReceived → process via agent, emit MessageReady
    ptrclaw::subscribe<ptrclaw::MessageReceivedEvent>(bus,
        [&sessions, &bus, &stream_states, &channel](const ptrclaw::MessageReceivedEvent& ev) {
            auto& agent = sessions.get_session(ev.session_id);
            std::string chat_id = ev.message.reply_target.value_or("");

            // Handle /start command
            if (ev.message.content == "/start") {
                ptrclaw::MessageReadyEvent reply;
                reply.session_id = ev.session_id;
                reply.reply_target = chat_id;
                std::string greeting = "Hello";
                if (ev.message.first_name) greeting += " " + *ev.message.first_name;
                greeting += "! I'm PtrClaw, an AI assistant. How can I help you?";
                reply.content = greeting;
                bus.publish(reply);
                return;
            }

            // Handle /new command
            if (ev.message.content == "/new") {
                agent.clear_history();
                ptrclaw::MessageReadyEvent reply;
                reply.session_id = ev.session_id;
                reply.reply_target = chat_id;
                reply.content = "Conversation cleared. What would you like to discuss?";
                bus.publish(reply);
                return;
            }

            // Show typing indicator immediately
            channel->send_typing_indicator(chat_id);

            // Register stream state before processing
            stream_states[ev.session_id] = {chat_id, 0, {},
                                             std::chrono::steady_clock::now(), false};

            std::string response = agent.process(ev.message.content);
            ptrclaw::MessageReadyEvent reply;
            reply.session_id = ev.session_id;
            reply.reply_target = chat_id;
            reply.content = response;
            bus.publish(reply);
        });

    // Refresh typing indicator on each tool call
    ptrclaw::subscribe<ptrclaw::ToolCallRequestEvent>(bus,
        [&channel, &stream_states](const ptrclaw::ToolCallRequestEvent& ev) {
            auto it = stream_states.find(ev.session_id);
            if (it != stream_states.end() && !it->second.chat_id.empty()) {
                channel->send_typing_indicator(it->second.chat_id);
            }
        });

    // Stream event subscribers (progressive message editing)
    if (channel->supports_streaming_display()) {
        ptrclaw::subscribe<ptrclaw::StreamStartEvent>(bus,
            [&channel, &stream_states](const ptrclaw::StreamStartEvent& ev) {
                auto it = stream_states.find(ev.session_id);
                if (it == stream_states.end()) return;
                int64_t msg_id = channel->send_streaming_placeholder(
                    it->second.chat_id);
                it->second.message_id = msg_id;
                it->second.last_edit = std::chrono::steady_clock::now();
            });

        ptrclaw::subscribe<ptrclaw::StreamChunkEvent>(bus,
            [&channel, &stream_states](const ptrclaw::StreamChunkEvent& ev) {
                auto it = stream_states.find(ev.session_id);
                if (it == stream_states.end() || it->second.message_id == 0)
                    return;
                it->second.accumulated += ev.delta;
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<
                    std::chrono::milliseconds>(
                    now - it->second.last_edit).count();
                if (elapsed >= 1000) {
                    channel->edit_message(it->second.chat_id,
                                          it->second.message_id,
                                          it->second.accumulated);
                    it->second.last_edit = now;
                }
            });

        ptrclaw::subscribe<ptrclaw::StreamEndEvent>(bus,
            [&channel, &stream_states](const ptrclaw::StreamEndEvent& ev) {
                auto it = stream_states.find(ev.session_id);
                if (it == stream_states.end() || it->second.message_id == 0)
                    return;
                channel->edit_message(it->second.chat_id,
                                      it->second.message_id,
                                      it->second.accumulated);
                it->second.delivered = true;
            });
    }

    uint32_t poll_count = 0;
    std::cerr << "[" << channel_name << "] Bot started. Polling for messages...\n";

    if (!channel->supports_polling()) {
        std::cerr << channel_name << " channel requires an external webhook gateway.\n"
                  << "Use the channel API programmatically with your HTTP server.\n";
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
        ptrclaw::http_set_abort_flag(&g_shutdown);

        ptrclaw::CurlHttpClient http_client;
        int rc = run_channel(channel_name, config, http_client);

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
