#include "config.hpp"
#include "provider.hpp"
#include "tool.hpp"
#include "agent.hpp"
#include "http.hpp"
#include <iostream>
#include <string>
#include <cstring>

static void print_usage() {
    std::cout << "Usage: ptrclaw [options]\n"
              << "\n"
              << "Options:\n"
              << "  -m, --message MSG    Send a single message and exit\n"
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
              << "  OLLAMA_BASE_URL      Base URL for Ollama (default: http://localhost:11434)\n";
}

int main(int argc, char* argv[]) {
    // Parse arguments
    std::string message;
    std::string provider_name;
    std::string model_name;

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

    // Create provider
    std::unique_ptr<ptrclaw::Provider> provider;
    try {
        provider = ptrclaw::create_provider(
            config.default_provider,
            config.api_key_for(config.default_provider),
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
        std::cout << response << std::endl;
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
}
