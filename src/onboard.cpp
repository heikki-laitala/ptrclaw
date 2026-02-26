#include "onboard.hpp"
#include "oauth.hpp"
#include "plugin.hpp"
#include "util.hpp"
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <nlohmann/json.hpp>

namespace ptrclaw {

const std::vector<std::string> kHiddenProviders = {"reliable", "compatible"};

bool is_hidden_provider(const std::string& name) {
    for (const auto& h : kHiddenProviders) {
        if (h == name) return true;
    }
    return false;
}

const char* provider_label(const std::string& name) {
    if (name == "anthropic") return "Anthropic (Claude)";
    if (name == "openai") return "OpenAI (GPT)";
    if (name == "openrouter") return "OpenRouter (multi-model)";
    if (name == "ollama") return "Ollama (local, no API key)";
    return name.c_str();
}

bool persist_provider_key(const std::string& provider, const std::string& api_key) {
    return modify_config_json([&](nlohmann::json& j) {
        if (!j.contains("providers") || !j["providers"].is_object())
            j["providers"] = nlohmann::json::object();
        if (!j["providers"].contains(provider) || !j["providers"][provider].is_object())
            j["providers"][provider] = nlohmann::json::object();
        j["providers"][provider]["api_key"] = api_key;
    });
}

std::string format_auth_status(const Config& config) {
    auto& reg = PluginRegistry::instance();
    std::string status = "Auth status:\n";
    for (const auto& name : reg.provider_names()) {
        if (is_hidden_provider(name)) continue;
        auto it = config.providers.find(name);
        status += "  ";
        status += provider_label(name);
        // Pad to align status column
        size_t label_len = std::string(provider_label(name)).size();
        for (size_t i = label_len; i < 26; ++i) status += ' ';
        if (it != config.providers.end() && name == "ollama") {
            status += it->second.base_url;
        } else if (it != config.providers.end() && it->second.use_oauth) {
            status += "OAuth";
            if (!it->second.oauth_access_token.empty())
                status += " (token present)";
        } else if (it != config.providers.end() && !it->second.api_key.empty()) {
            status += "API key";
        } else {
            status += "not configured";
        }
        status += "\n";
    }
    return status;
}

namespace {

// Default model for each provider
const char* default_model(const std::string& name) {
    if (name == "anthropic") return "claude-sonnet-4-6";
    if (name == "openai") return "gpt-4o-mini";
    if (name == "openrouter") return "openrouter/auto";
    if (name == "ollama") return "llama3.2";
    return "";
}

bool is_local_provider(const std::string& name) {
    return name == "ollama";
}

// Read a line from stdin, returns false on EOF
bool read_line(std::string& out) {
    if (!std::getline(std::cin, out)) return false;
    out = trim(out);
    return true;
}

// Read y/n answer (default_yes = true means Enter → yes)
bool ask_yes_no(bool default_yes = true) {
    std::string answer;
    if (!read_line(answer)) return false;
    if (answer.empty()) return default_yes;
    return answer[0] == 'y' || answer[0] == 'Y';
}

// Read a number choice (1-based), returns 0 on invalid/EOF
int read_choice(int max) {
    std::string line;
    if (!read_line(line)) return 0;
    try {
        int n = std::stoi(line);
        if (n >= 1 && n <= max) return n;
    } catch (...) { return 0; }
    return 0;
}

// Persist a channel's config to config.json
bool persist_channel_token(const std::string& channel,
                           const nlohmann::json& channel_json) {
    return modify_config_json([&](nlohmann::json& j) {
        if (!j.contains("channels") || !j["channels"].is_object())
            j["channels"] = nlohmann::json::object();
        if (!j["channels"].contains(channel) || !j["channels"][channel].is_object())
            j["channels"][channel] = nlohmann::json::object();
        for (auto& [key, val] : channel_json.items()) {
            j["channels"][channel][key] = val;
        }
    });
}

// OpenAI OAuth inline flow — returns true if OAuth was completed
bool setup_openai_oauth(Config& config, HttpClient& http) {
    auto flow = start_oauth_flow(config.providers["openai"]);

    std::cout << "\nOpen this URL to authorize:\n" << flow.authorize_url
              << "\n\nPaste the callback URL or code: " << std::flush;

    std::string input;
    if (!read_line(input) || input.empty()) {
        std::cout << "Skipped.\n";
        return false;
    }

    auto parsed = parse_oauth_input(input);
    if (parsed.code.empty()) {
        std::cout << "Could not extract auth code.\n";
        return false;
    }
    if (!parsed.state.empty() && parsed.state != flow.pending.state) {
        std::cout << "State mismatch. Please try again.\n";
        return false;
    }

    auto result = apply_oauth_result(parsed.code, flow.pending, config, http);
    if (!result.success) {
        std::cout << result.error << "\n";
        return false;
    }

    config.provider = "openai";
    config.model = kDefaultOAuthModel;
    config.persist_selection();
    std::cout << "OAuth connected. Provider: openai | Model: "
              << kDefaultOAuthModel << "\n";
    if (!result.persisted) {
        std::cout << "(warning: could not persist to config file)\n";
    }
    return true;
}

// Step 1: Provider setup
bool setup_provider(Config& config, HttpClient& http) {
    auto& reg = PluginRegistry::instance();
    auto all_names = reg.provider_names();

    // Filter to user-facing providers
    std::vector<std::string> names;
    for (const auto& name : all_names) {
        if (!is_hidden_provider(name)) names.push_back(name);
    }

    // Sort: anthropic, openai, openrouter, ollama
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
        auto rank = [](const std::string& n) -> int {
            if (n == "anthropic") return 0;
            if (n == "openai") return 1;
            if (n == "openrouter") return 2;
            if (n == "ollama") return 3;
            return 4;
        };
        return rank(a) < rank(b);
    });

    if (names.empty()) {
        std::cout << "No providers available. Build with provider support enabled.\n";
        return false;
    }

    std::cout << "Choose a provider:\n";
    for (size_t i = 0; i < names.size(); ++i) {
        std::cout << "  " << (i + 1) << ". " << provider_label(names[i]) << "\n";
    }
    std::cout << "> " << std::flush;

    int choice = read_choice(static_cast<int>(names.size()));
    if (choice == 0) {
        std::cout << "Invalid choice.\n";
        return false;
    }

    std::string chosen = names[static_cast<size_t>(choice - 1)];

    if (is_local_provider(chosen)) {
        // Ollama: confirm base URL
        auto it = config.providers.find(chosen);
        std::string current_url = (it != config.providers.end())
            ? it->second.base_url : "http://localhost:11434";
        std::cout << "Base URL [" << current_url << "]: " << std::flush;
        std::string url;
        read_line(url);
        if (url.empty()) url = current_url;
        config.providers[chosen].base_url = url;
    } else {
        // OpenAI: offer OAuth as alternative to API key
        if (chosen == "openai") {
            std::cout << "Authentication method:\n"
                      << "  1. API key\n"
                      << "  2. OAuth login (ChatGPT subscription)\n"
                      << "> " << std::flush;
            int auth_choice = read_choice(2);
            if (auth_choice == 2) {
                return setup_openai_oauth(config, http);
            }
        }

        std::cout << "Enter your " << provider_label(chosen) << " API key: " << std::flush;
        std::string api_key;
        if (!read_line(api_key) || api_key.empty()) {
            std::cout << "No API key provided.\n";
            return false;
        }
        config.providers[chosen].api_key = api_key;
        persist_provider_key(chosen, api_key);
    }

    config.provider = chosen;
    const char* model = default_model(chosen);
    if (model[0] != '\0') config.model = model;
    config.persist_selection();

    std::cout << "Saved. Provider: " << chosen
              << " | Model: " << config.model << "\n";
    return true;
}

// Step 2: Channel setup
void setup_channel(Config& config, HttpClient& http) {
    auto& reg = PluginRegistry::instance();
    auto all_channels = reg.channel_names();

    if (all_channels.empty()) return;

    std::cout << "\nWould you like to set up a messaging channel? (y/n) [y]: "
              << std::flush;
    if (!ask_yes_no(true)) return;

    std::cout << "\nAvailable channels:\n";
    for (size_t i = 0; i < all_channels.size(); ++i) {
        std::cout << "  " << (i + 1) << ". " << all_channels[i] << "\n";
    }
    std::cout << "> " << std::flush;

    int choice = read_choice(static_cast<int>(all_channels.size()));
    if (choice == 0) {
        std::cout << "Skipped.\n";
        return;
    }

    const std::string& chosen = all_channels[static_cast<size_t>(choice - 1)];

    if (chosen == "telegram") {
        std::cout << "Enter your Telegram bot token: " << std::flush;
        std::string token;
        if (!read_line(token) || token.empty()) {
            std::cout << "Skipped.\n";
            return;
        }

        // Store token in config and validate
        config.channels["telegram"]["bot_token"] = token;
        std::cout << "Validating... " << std::flush;

        try {
            auto channel = reg.create_channel("telegram", config, http);
            if (channel->health_check()) {
                std::cout << "OK!\n";
                persist_channel_token("telegram", {{"bot_token", token}});
                std::cout << "Saved. Run with --channel telegram to start the bot.\n";
            } else {
                std::cout << "Failed. Check your bot token.\n";
                config.channels["telegram"]["bot_token"] = "";
            }
        } catch (const std::exception& e) {
            std::cout << "Error: " << e.what() << "\n";
            config.channels["telegram"]["bot_token"] = "";
        }
    } else if (chosen == "whatsapp") {
        std::cout << "Enter your WhatsApp access token: " << std::flush;
        std::string access_token;
        if (!read_line(access_token) || access_token.empty()) {
            std::cout << "Skipped.\n";
            return;
        }

        std::cout << "Enter your WhatsApp phone number ID: " << std::flush;
        std::string phone_id;
        if (!read_line(phone_id) || phone_id.empty()) {
            std::cout << "Skipped.\n";
            return;
        }

        std::cout << "Enter your WhatsApp verify token: " << std::flush;
        std::string verify_token;
        if (!read_line(verify_token) || verify_token.empty()) {
            std::cout << "Skipped.\n";
            return;
        }

        config.channels["whatsapp"]["access_token"] = access_token;
        config.channels["whatsapp"]["phone_number_id"] = phone_id;
        config.channels["whatsapp"]["verify_token"] = verify_token;

        nlohmann::json ch_json = {
            {"access_token", access_token},
            {"phone_number_id", phone_id},
            {"verify_token", verify_token}
        };
        persist_channel_token("whatsapp", ch_json);
        std::cout << "Saved. Run with --channel whatsapp to start the bot.\n";
    } else {
        std::cout << "Channel '" << chosen << "' setup not yet supported.\n";
    }
}

// Step 3: Hatch offer
void offer_hatch(bool& hatch_requested) {
    auto& reg = PluginRegistry::instance();
    if (!reg.has_memory("json") && !reg.has_memory("sqlite")) return;

    std::cout << "\nWould you like to create a personality for your assistant? (y/n) [y]: "
              << std::flush;
    hatch_requested = ask_yes_no(true);
}

} // namespace

bool run_onboard(Config& config, HttpClient& http, bool& hatch_requested) {
    std::cout << "Welcome to PtrClaw setup!\n\n";

    hatch_requested = false;

    if (!setup_provider(config, http)) {
        return false;
    }

    setup_channel(config, http);
    offer_hatch(hatch_requested);

    std::cout << "\nSetup complete!\n";
    return true;
}

bool needs_onboard(const Config& config) {
    // If a provider is already selected, trust that choice
    if (!config.provider.empty()) {
        // Local providers (Ollama) don't need credentials
        if (config.provider == "ollama") return false;
        // Check if the selected provider has credentials
        auto it = config.providers.find(config.provider);
        if (it != config.providers.end()) {
            if (!it->second.api_key.empty()) return false;
            if (!it->second.oauth_access_token.empty()) return false;
        }
    }
    // No provider selected — check if any provider has credentials
    for (const auto& [name, entry] : config.providers) {
        if (!entry.api_key.empty()) return false;
        if (!entry.oauth_access_token.empty()) return false;
    }
    return true;
}

} // namespace ptrclaw
