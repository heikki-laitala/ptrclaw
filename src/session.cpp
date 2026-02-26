#include "session.hpp"
#include "oauth.hpp"
#include "event.hpp"
#include "event_bus.hpp"
#include "prompt.hpp"
#include "util.hpp"
#include "providers/openai.hpp"

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
    auto provider_it = config_.providers.find(config_.provider);
    auto provider = create_provider(
        config_.provider,
        config_.api_key_for(config_.provider),
        http_,
        config_.base_url_for(config_.provider),
        config_.prompt_caching_for(config_.provider),
        provider_it != config_.providers.end() ? &provider_it->second : nullptr);
    setup_oauth_refresh_callback(provider.get());

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

void SessionManager::setup_oauth_refresh_callback(Provider* provider) {
    auto* oai = dynamic_cast<OpenAIProvider*>(provider);
    if (!oai) return;
    oai->set_on_token_refresh(
        [this](const std::string& at, const std::string& rt, uint64_t ea) {
            auto& entry = config_.providers["openai"];
            entry.oauth_access_token = at;
            entry.oauth_refresh_token = rt;
            entry.oauth_expires_at = ea;
            persist_openai_oauth(entry);
        });
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
                send_reply(agent.process(
                    "The user wants to start hatching. Begin the interview."));
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

            // Handle /models command
            if (ev.message.content == "/models") {
                auto infos = list_providers(config_, agent.provider_name());

                std::string result = "Configured providers:\n";
                for (const auto& info : infos) {
                    result += info.active ? "* " : "  ";
                    result += info.name;
                    if (info.has_api_key && info.has_oauth)
                        result += " — API key + OAuth";
                    else if (info.has_api_key)
                        result += " — API key";
                    else if (info.has_oauth)
                        result += " — OAuth";
                    else if (info.is_local)
                        result += " — local";
                    if (info.active) result += "  [model: " + agent.model() + "]";
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
                    setup_oauth_refresh_callback(sr.provider.get());
                    agent.set_provider(std::move(sr.provider));
                    if (!sr.model.empty()) agent.set_model(sr.model);
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
        setup_oauth_refresh_callback(r.provider.get());
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

        if (parts.size() >= 2 && parts[1] == "status") {
            auto it = config_.providers.find("openai");
            if (it == config_.providers.end()) {
                send_reply("No OpenAI provider config found.");
                return true;
            }
            const auto& openai = it->second;
            std::string status = "OpenAI auth: ";
            if (openai.use_oauth) {
                status += "OAuth enabled";
                if (!openai.oauth_access_token.empty()) {
                    status += " (token present)";
                }
                if (openai.oauth_expires_at > 0) {
                    status += "\nExpires at (epoch): " + std::to_string(openai.oauth_expires_at);
                }
            } else if (!openai.api_key.empty()) {
                status += "API key";
            } else {
                status += "not configured";
            }
            send_reply(status);
            return true;
        }

        if (parts.size() >= 3 && parts[1] == "openai" && parts[2] == "start") {
            auto openai_it = config_.providers.find("openai");
            if (openai_it == config_.providers.end()) {
                send_reply("OpenAI provider config missing.");
                return true;
            }

            std::string state = generate_id();
            std::string verifier = make_code_verifier();
            std::string challenge = make_code_challenge_s256(verifier);
            std::string client_id = openai_it->second.oauth_client_id.empty()
                ? kDefaultOAuthClientId
                : openai_it->second.oauth_client_id;

            PendingOAuth pending;
            pending.provider = "openai";
            pending.state = state;
            pending.code_verifier = verifier;
            pending.redirect_uri = kDefaultRedirectUri;
            pending.created_at = epoch_seconds();
            set_pending_oauth(ev.session_id, std::move(pending));

            std::string url = build_authorize_url(client_id, kDefaultRedirectUri,
                                                   challenge, state);

            send_reply(
                "Open this URL to authorize OpenAI:\n" + url +
                "\n\nThen paste the full callback URL with:\n"
                "/auth openai finish <callback_url>\n"
                "(or paste just the code)");
            return true;
        }

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

        send_reply("Auth commands:\n/auth status\n/auth openai start\n/auth openai finish <callback_url_or_code>");
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
