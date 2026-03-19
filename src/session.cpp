#include "session.hpp"
#include "commands.hpp"
#include "embedder.hpp"
#include "onboard.hpp"
#include "event.hpp"
#include "event_bus.hpp"
#include "tool_manager.hpp"
#include "plugin.hpp"
#include "util.hpp"
#ifdef PTRCLAW_HAS_OPENAI
#include "providers/oauth_openai.hpp"
#endif

namespace ptrclaw {

SessionManager::SessionManager(Config& config, HttpClient& http)
    : config_(config), http_(http)
{}

Session SessionManager::create_session(const std::string& session_id) {
    auto sr = switch_provider(
        config_.provider, config_.model, config_.model, config_, http_);
    if (!sr.provider) {
        throw std::runtime_error("Cannot create provider: " + sr.error);
    }

    Session session;
    session.id = session_id;
    session.agent = std::make_unique<Agent>(std::move(sr.provider), config_);
    session.last_active = epoch_seconds();

    if (!binary_path_.empty()) {
        session.agent->set_binary_path(binary_path_);
    }

    if (event_bus_) {
        session.agent->set_event_bus(event_bus_);
        session.agent->set_session_id(session_id);

        auto tools = create_builtin_tools();
        session.tool_manager = std::make_unique<ToolManager>(
            std::move(tools), config_, *event_bus_, session_id);
        session.tool_manager->wire_memory(session.agent->memory());
        session.tool_manager->publish_tool_specs(session_id);

        SessionCreatedEvent ev;
        ev.session_id = session_id;
        event_bus_->publish(ev);
    }

    if (embedder_) {
        session.agent->set_embedder(embedder_);
    }

    return session;
}

Agent& SessionManager::get_session(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = sessions_.find(session_id);
    if (it != sessions_.end()) {
        it->second.last_active = epoch_seconds();
        return *(it->second.agent);
    }

    auto [inserted, _] = sessions_.emplace(session_id, create_session(session_id));
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

#ifdef PTRCLAW_HAS_OPENAI
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
#endif

void SessionManager::handle_message(const MessageReceivedEvent& ev) {
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

    // Handle /new and /clear commands
    if (ev.message.content == "/new" || ev.message.content == "/clear") {
        agent.clear_history();
        send_reply("Conversation history cleared. What would you like to discuss?");
        return;
    }

    // Handle /soul command — developer-only
    if (ev.message.content == "/soul") {
        send_reply(cmd_soul(agent, config_.dev));
        return;
    }

    // Handle /hatch command
    if (ev.message.content == "/hatch") {
        send_reply(cmd_hatch(agent));
        return;
    }

    // Handle /status command
    if (ev.message.content == "/status") {
        send_reply(cmd_status(agent));
        return;
    }

    // Handle /model command
    if (ev.message.content.rfind("/model ", 0) == 0) {
        send_reply(cmd_model(trim(ev.message.content.substr(7)),
                             agent, config_, http_));
        return;
    }

    // Handle /memory commands
    if (ev.message.content == "/memory") {
        send_reply(cmd_memory(agent));
        return;
    }
    if (ev.message.content == "/memory export") {
        send_reply(cmd_memory_export(agent));
        return;
    }
    if (ev.message.content.rfind("/memory import ", 0) == 0) {
        send_reply(cmd_memory_import(agent, trim(ev.message.content.substr(15))));
        return;
    }

    // Handle /skill command
    if (ev.message.content == "/skill" ||
        ev.message.content.rfind("/skill ", 0) == 0) {
        std::string args = (ev.message.content.size() > 7)
            ? ev.message.content.substr(7) : "";
        send_reply(cmd_skill(args, agent));
        return;
    }

    // Handle /help command
    if (ev.message.content == "/help") {
        bool is_channel = (ev.session_id != kCliSessionId);
        send_reply(cmd_help(config_.dev, is_channel));
        return;
    }

    // Handle /models command
    if (ev.message.content == "/models") {
        send_reply(cmd_models(agent, config_));
        return;
    }

    // Handle /provider command
    if (ev.message.content.rfind("/provider ", 0) == 0) {
        send_reply(cmd_provider(ev.message.content.substr(10),
                                agent, config_, http_));
        return;
    }

    // Handle auth commands + raw OAuth paste
    if (ev.message.content.rfind("/auth", 0) == 0
#ifdef PTRCLAW_HAS_OPENAI
        || get_pending_oauth(ev.session_id).has_value()
#endif
    ) {
        if (handle_auth_command(ev, agent, send_reply)) return;
    }

    // Auto-hatch: if memory exists but no soul, enter hatching
    // so the user's first message kicks off the interview
    if (agent.memory() && !agent.is_hatched() && !agent.hatching()) {
        agent.start_hatch();
    }

    send_reply(agent.process(ev.message.content));
}

void SessionManager::subscribe_events() {
    if (!event_bus_) return;

    ptrclaw::subscribe<MessageReceivedEvent>(*event_bus_,
        [this](const MessageReceivedEvent& ev) {
            handle_message(ev);
        });
}

bool SessionManager::handle_auth_command(
    const MessageReceivedEvent& ev,
    Agent& agent,
    const std::function<void(const std::string&)>& send_reply) {

#ifdef PTRCLAW_HAS_OPENAI
    auto finish_oauth = [&](const PendingOAuth& pending,
                             const std::string& code) {
        auto r = apply_oauth_result(code, pending, config_, http_);
        if (!r.success) { send_reply(r.error); return; }
        agent.set_provider(std::move(r.provider));
        agent.set_model(kDefaultOAuthModel);
        clear_pending_oauth(ev.session_id);
        send_reply(std::string("OpenAI OAuth connected ✅ Model switched to ") +
                   kDefaultOAuthModel + "." +
                   (r.persisted
                    ? " Saved to ~/.ptrclaw/config.json"
                    : " (warning: could not persist to config file)"));
    };
#endif

    // Handle /auth commands
    if (ev.message.content.rfind("/auth", 0) == 0) {
        auto parts = split(ev.message.content, ' ');

#ifdef PTRCLAW_HAS_OPENAI
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
#endif

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

#ifdef PTRCLAW_HAS_OPENAI
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
#endif

    return false;
}

} // namespace ptrclaw
