#include "session.hpp"
#include "embedder.hpp"
#include "oauth.hpp"
#include "onboard.hpp"
#include "event.hpp"
#include "event_bus.hpp"
#include "plugin.hpp"
#include "prompt.hpp"
#include "util.hpp"
#include <nlohmann/json.hpp>

namespace ptrclaw {

SessionManager::SessionManager(const Config& config, HttpClient& http)
    : config_(config), http_(http)
{}

Agent& SessionManager::get_session(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = sessions_.find(session_id);
    if (it != sessions_.end()) {
        it->second.last_active = epoch_seconds();
        return *(it->second.agent);
    }

    // Create new session
    auto sr = switch_provider(
        config_.provider, config_.model, config_.model, config_, http_);
    if (!sr.provider) {
        throw std::runtime_error("Cannot create provider: " + sr.error);
    }
    auto provider = std::move(sr.provider);
    setup_oauth_refresh(provider.get(), config_);

    auto tools = create_builtin_tools();

    Session session;
    session.id = session_id;
    session.agent = std::make_unique<Agent>(std::move(provider), std::move(tools), config_);
    session.last_active = epoch_seconds();

    // Propagate binary path to new agent
    if (!binary_path_.empty()) {
        session.agent->set_binary_path(binary_path_);
    }

    // Propagate event bus to new agent
    if (event_bus_) {
        session.agent->set_event_bus(event_bus_);
        session.agent->set_session_id(session_id);

        SessionCreatedEvent ev;
        ev.session_id = session_id;
        event_bus_->publish(ev);
    }

    // Propagate embedder to new agent (shared, non-owning)
    if (embedder_) {
        session.agent->set_embedder(embedder_);
    }

    auto [inserted, _] = sessions_.emplace(session_id, std::move(session));
    return *(inserted->second.agent);
}

void SessionManager::remove_session(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.erase(session_id);
}

void SessionManager::evict_idle(uint64_t max_idle_seconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t now = epoch_seconds();

    for (auto it = sessions_.begin(); it != sessions_.end(); ) {
        if ((now - it->second.last_active) > max_idle_seconds) {
            if (event_bus_) {
                SessionEvictedEvent ev;
                ev.session_id = it->first;
                event_bus_->publish(ev);
            }
            it = sessions_.erase(it);
        } else {
            ++it;
        }
    }
}

std::vector<std::string> SessionManager::list_sessions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> ids;
    ids.reserve(sessions_.size());
    for (const auto& [id, _] : sessions_) {
        ids.push_back(id);
    }
    return ids;
}

std::optional<PendingOAuth> SessionManager::get_pending_oauth(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pending_oauth_.find(session_id);
    if (it == pending_oauth_.end()) return std::nullopt;
    if (it->second.created_at > 0 &&
        epoch_seconds() - it->second.created_at > kPendingOAuthExpirySeconds) {
        pending_oauth_.erase(it);
        return std::nullopt;
    }
    return it->second;
}

void SessionManager::set_pending_oauth(const std::string& session_id, PendingOAuth pending) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_oauth_[session_id] = std::move(pending);
}

void SessionManager::clear_pending_oauth(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_oauth_.erase(session_id);
}


void SessionManager::subscribe_events() {
    if (!event_bus_) return;

    ptrclaw::subscribe<MessageReceivedEvent>(*event_bus_,
        [this](const MessageReceivedEvent& ev) {
            auto& agent = get_session(ev.session_id);
            if (!ev.message.channel.empty()) {
                agent.set_channel(ev.message.channel);
            }
            std::string chat_id = ev.message.reply_target.value_or("");

            auto send_reply = [&](const std::string& content) {
                MessageReadyEvent reply;
                reply.session_id = ev.session_id;
                reply.reply_target = chat_id;
                reply.content = content;
                event_bus_->publish(reply);
            };

            auto begin_hatch = [&]() {
                agent.start_hatch();
                send_reply(agent.process("Begin the hatching interview."));
            };

            // Handle /start command
            if (ev.message.content == "/start") {
                if (agent.memory() && !agent.is_hatched()) {
                    begin_hatch();
                } else {
                    std::string greeting = "Hello";
                    if (ev.message.first_name) greeting += " " + *ev.message.first_name;
                    greeting += "! I'm PtrClaw, an AI assistant. How can I help you?";
                    send_reply(greeting);
                }
                return;
            }

            // Handle /new command
            if (ev.message.content == "/new") {
                agent.clear_history();
                send_reply("Conversation history cleared. What would you like to discuss?");
                return;
            }

            // Handle /soul command — developer-only
            if (ev.message.content == "/soul") {
                if (!config_.dev) {
                    send_reply("Unknown command: /soul");
                    return;
                }
                std::string display;
                if (agent.memory()) {
                    display = format_soul_display(agent.memory());
                }
                send_reply(display.empty()
                    ? "No soul data yet. Use /hatch to create one."
                    : display);
                return;
            }

            // Handle /hatch command
            if (ev.message.content == "/hatch") {
                begin_hatch();
                return;
            }

            // Handle /status command
            if (ev.message.content == "/status") {
                std::string result = "Provider: " + agent.provider_name() + "\n"
                    + "Model: " + agent.model() + "\n"
                    + "History: " + std::to_string(agent.history_size()) + " messages\n"
                    + "Estimated tokens: " + std::to_string(agent.estimated_tokens()) + "\n";
                send_reply(result);
                return;
            }

            // Handle /model command
            if (ev.message.content.rfind("/model ", 0) == 0) {
                std::string new_model = trim(ev.message.content.substr(7));
                // Re-create provider if switching between OAuth and API-key mode
                if (agent.provider_name() == "openai") {
                    auto oai_it = config_.providers.find("openai");
                    bool on_oauth = oai_it != config_.providers.end() &&
                                    oai_it->second.use_oauth;
                    bool want_oauth = new_model.find("codex") != std::string::npos &&
                                      oai_it != config_.providers.end() &&
                                      !oai_it->second.oauth_access_token.empty();
                    if (on_oauth != want_oauth) {
                        auto sr = switch_provider("openai", new_model, agent.model(),
                                                   config_, http_);
                        if (!sr.error.empty()) {
                            send_reply(sr.error);
                        } else {
                            setup_oauth_refresh(sr.provider.get(), config_);
                            agent.set_provider(std::move(sr.provider));
                            if (!sr.model.empty()) agent.set_model(sr.model);
                            config_.model = agent.model();
                            config_.persist_selection();
                            send_reply("Model set to: " + agent.model());
                        }
                        return;
                    }
                }
                agent.set_model(new_model);
                config_.model = new_model;
                config_.persist_selection();
                send_reply("Model set to: " + new_model);
                return;
            }

            // Handle /memory command
            if (ev.message.content == "/memory") {
                auto* mem = agent.memory();
                if (!mem || mem->backend_name() == "none") {
                    send_reply("Memory: disabled");
                } else {
                    std::string result = "Memory backend: "
                        + std::string(mem->backend_name()) + "\n"
                        + "  Core:         "
                        + std::to_string(mem->count(MemoryCategory::Core)) + " entries\n"
                        + "  Knowledge:    "
                        + std::to_string(mem->count(MemoryCategory::Knowledge)) + " entries\n"
                        + "  Conversation: "
                        + std::to_string(mem->count(MemoryCategory::Conversation)) + " entries\n"
                        + "  Total:        "
                        + std::to_string(mem->count(std::nullopt)) + " entries\n";
                    send_reply(result);
                }
                return;
            }

            // Handle /help command
            if (ev.message.content == "/help") {
                std::string help = "Commands:\n"
                    "  /new             Clear conversation history\n"
                    "  /status          Show current status\n"
                    "  /model X         Switch to model X\n"
                    "  /models          List configured providers\n"
                    "  /provider X [M]  Switch to provider X, optional model M\n"
                    "  /memory          Show memory status\n"
                    "  /auth            Show auth status for all providers\n"
                    "  /auth <prov> <key>  Set API key\n"
                    "  /auth openai start  Begin OAuth flow\n"
                    "  /hatch           Create or re-create assistant identity\n"
                    "  /help            Show this help\n";
                send_reply(help);
                return;
            }

            // Handle /models command
            if (ev.message.content == "/models") {
                std::string auth_mode = auth_mode_label(
                    agent.provider_name(), agent.model(), config_);
                std::string result = "Current: ";
                result += agent.provider_name();
                result += " — ";
                result += agent.model();
                result += " (";
                result += auth_mode;
                result += ")\n\nProviders:\n";

                // Provider list
                auto infos = list_providers(config_, agent.provider_name());
                for (const auto& info : infos) {
                    result += "  ";
                    result += info.name;
                    result += " — ";
                    if (info.has_api_key) result += "API key";
                    if (info.has_api_key && info.has_oauth) result += ", ";
                    if (info.has_oauth) result += "OAuth (codex models)";
                    if (info.is_local) result += "local";
                    result += "\n";
                }
                result += "\nSwitch: /provider <name> [model]";
                send_reply(result);
                return;
            }

            // Handle /provider command
            if (ev.message.content.rfind("/provider ", 0) == 0) {
                auto args = trim(ev.message.content.substr(10));
                auto space = args.find(' ');
                std::string prov_name = (space == std::string::npos) ? args : args.substr(0, space);
                std::string model_arg = (space == std::string::npos) ? "" : trim(args.substr(space + 1));

                auto sr = switch_provider(prov_name, model_arg, agent.model(), config_, http_);
                if (!sr.error.empty()) {
                    send_reply(sr.error);
                } else {
                    setup_oauth_refresh(sr.provider.get(), config_);
                    agent.set_provider(std::move(sr.provider));
                    if (!sr.model.empty()) agent.set_model(sr.model);
                    config_.provider = prov_name;
                    config_.model = agent.model();
                    config_.persist_selection();
                    send_reply("Switched to " + prov_name + " | Model: " + agent.model());
                }
                return;
            }

            // Handle auth commands + raw OAuth paste
            if (ev.message.content.rfind("/auth", 0) == 0 ||
                get_pending_oauth(ev.session_id).has_value()) {
                if (handle_auth_command(ev, agent, send_reply)) return;
            }

            // Auto-hatch: if memory exists but no soul, enter hatching
            // so the user's first message kicks off the interview
            if (agent.memory() && !agent.is_hatched() && !agent.hatching()) {
                agent.start_hatch();
            }

            send_reply(agent.process(ev.message.content));
        });
}

bool SessionManager::handle_auth_command(
    const MessageReceivedEvent& ev,
    Agent& agent,
    const std::function<void(const std::string&)>& send_reply) {

    auto finish_oauth = [&](const PendingOAuth& pending,
                             const std::string& code) {
        auto r = apply_oauth_result(code, pending, config_, http_);
        if (!r.success) { send_reply(r.error); return; }
        setup_oauth_refresh(r.provider.get(), config_);
        agent.set_provider(std::move(r.provider));
        agent.set_model(kDefaultOAuthModel);
        clear_pending_oauth(ev.session_id);
        send_reply(std::string("OpenAI OAuth connected ✅ Model switched to ") +
                   kDefaultOAuthModel + "." +
                   (r.persisted
                    ? " Saved to ~/.ptrclaw/config.json"
                    : " (warning: could not persist to config file)"));
    };

    // Handle /auth commands
    if (ev.message.content.rfind("/auth", 0) == 0) {
        auto parts = split(ev.message.content, ' ');

        // /auth openai start — two-step OAuth flow
        if (parts.size() >= 3 && parts[1] == "openai" && parts[2] == "start") {
            auto openai_it = config_.providers.find("openai");
            if (openai_it == config_.providers.end()) {
                send_reply("OpenAI provider config missing.");
                return true;
            }

            auto flow = start_oauth_flow(openai_it->second);
            set_pending_oauth(ev.session_id, std::move(flow.pending));

            send_reply(
                "Open this URL to authorize OpenAI:\n" + flow.authorize_url +
                "\n\nThen paste the full callback URL with:\n"
                "/auth openai finish <callback_url>\n"
                "(or paste just the code)");
            return true;
        }

        // /auth openai finish <url_or_code>
        if (parts.size() >= 4 && parts[1] == "openai" && parts[2] == "finish") {
            auto pending = get_pending_oauth(ev.session_id);
            if (!pending || pending->provider != "openai") {
                send_reply("No pending OpenAI auth flow. Start with: /auth openai start");
                return true;
            }

            std::string input = ev.message.content.substr(
                ev.message.content.find("finish") + 7);
            auto parsed = parse_oauth_input(input);

            if (parsed.code.empty()) {
                send_reply("Missing code. Paste callback URL or auth code.");
                return true;
            }
            if (!parsed.state.empty() && parsed.state != pending->state) {
                send_reply("State mismatch. Please restart with /auth openai start");
                return true;
            }

            finish_oauth(*pending, parsed.code);
            return true;
        }

        // /auth <provider> <key> — set API key for any provider
        if (parts.size() >= 3) {
            const std::string& prov = parts[1];
            bool known = false;
            for (const auto& n : PluginRegistry::instance().provider_names()) {
                if (n == prov) { known = true; break; }
            }
            if (is_hidden_provider(prov)) known = false;
            if (!known) {
                send_reply("Unknown provider: " + prov);
            } else if (prov == "ollama") {
                send_reply("Ollama is local and doesn't need an API key. "
                           "Set base_url in ~/.ptrclaw/config.json");
            } else {
                const std::string& api_key = parts[2];
                config_.providers[prov].api_key = api_key;
                persist_provider_key(prov, api_key);
                send_reply("API key saved for " + std::string(provider_label(prov)) + ".");
            }
            return true;
        }

        // /auth — show status
        send_reply(format_auth_status(config_) +
                   "\nSet credentials: /auth <provider> <api_key>\n"
                   "OAuth: /auth openai start");
        return true;
    }

    // If OpenAI OAuth is pending, accept raw callback URL/code directly.
    auto pending_oauth = get_pending_oauth(ev.session_id);
    if (pending_oauth && pending_oauth->provider == "openai") {
        std::string raw = trim(ev.message.content);
        bool looks_like_oauth_reply =
            (!raw.empty() &&
             (raw.find("code=") != std::string::npos ||
              raw.find("auth/callback") != std::string::npos ||
              raw.find("localhost:1455") != std::string::npos));

        if (looks_like_oauth_reply && raw.rfind("/auth", 0) != 0) {
            auto parsed = parse_oauth_input(raw);

            if (parsed.code.empty()) {
                send_reply("Missing code. Paste callback URL or auth code.");
                return true;
            }
            if (!parsed.state.empty() && parsed.state != pending_oauth->state) {
                send_reply("State mismatch. Please restart with /auth openai start");
                return true;
            }

            finish_oauth(*pending_oauth, parsed.code);
            return true;
        }
    }

    return false;
}

} // namespace ptrclaw
