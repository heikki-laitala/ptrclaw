#include "session.hpp"
#include "event.hpp"
#include "event_bus.hpp"
#include "prompt.hpp"
#include "util.hpp"

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
                send_reply("Conversation cleared. What would you like to discuss?");
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

            // Auto-hatch: if memory exists but no soul, enter hatching
            // so the user's first message kicks off the interview
            if (agent.memory() && !agent.is_hatched() && !agent.hatching()) {
                agent.start_hatch();
            }

            send_reply(agent.process(ev.message.content));
        });
}

} // namespace ptrclaw
