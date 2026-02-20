#include "session.hpp"
#include "event.hpp"
#include "event_bus.hpp"
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
    auto provider = create_provider(
        config_.default_provider,
        config_.api_key_for(config_.default_provider),
        http_,
        config_.ollama_base_url);

    auto tools = create_builtin_tools();

    Session session;
    session.id = session_id;
    session.agent = std::make_unique<Agent>(std::move(provider), std::move(tools), config_);
    session.last_active = epoch_seconds();

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
            std::string chat_id = ev.message.reply_target.value_or("");

            // Handle /start command
            if (ev.message.content == "/start") {
                MessageReadyEvent reply;
                reply.session_id = ev.session_id;
                reply.reply_target = chat_id;
                std::string greeting = "Hello";
                if (ev.message.first_name) greeting += " " + *ev.message.first_name;
                greeting += "! I'm PtrClaw, an AI assistant. How can I help you?";
                reply.content = greeting;
                event_bus_->publish(reply);
                return;
            }

            // Handle /new command
            if (ev.message.content == "/new") {
                agent.clear_history();
                MessageReadyEvent reply;
                reply.session_id = ev.session_id;
                reply.reply_target = chat_id;
                reply.content = "Conversation cleared. What would you like to discuss?";
                event_bus_->publish(reply);
                return;
            }

            std::string response = agent.process(ev.message.content);
            MessageReadyEvent reply;
            reply.session_id = ev.session_id;
            reply.reply_target = chat_id;
            reply.content = response;
            event_bus_->publish(reply);
        });
}

} // namespace ptrclaw
