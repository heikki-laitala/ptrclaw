#pragma once
#include "agent.hpp"
#include "config.hpp"
#include "http.hpp"
#include <string>
#include <memory>
#include <unordered_map>
#include <mutex>
#include <cstdint>
#include <optional>
#include <functional>

namespace ptrclaw {

class EventBus; // forward declaration
struct MessageReceivedEvent; // forward declaration

struct Session {
    std::string id;
    std::unique_ptr<Agent> agent;
    uint64_t last_active = 0;
};

constexpr uint64_t kPendingOAuthExpirySeconds = 600; // 10 minutes

struct PendingOAuth {
    std::string provider;
    std::string state;
    std::string code_verifier;
    std::string redirect_uri;
    uint64_t created_at = 0;
};

class SessionManager {
public:
    SessionManager(const Config& config, HttpClient& http);

    // Get or create a session
    Agent& get_session(const std::string& session_id);

    // Remove a session
    void remove_session(const std::string& session_id);

    // Evict idle sessions (older than max_idle_seconds)
    void evict_idle(uint64_t max_idle_seconds = 3600);

    // List active session IDs
    std::vector<std::string> list_sessions() const;

    // Binary path — propagated to new agents for cron scheduling
    void set_binary_path(const std::string& path) { binary_path_ = path; }

    // Optional event bus — propagated to new agents
    void set_event_bus(EventBus* bus) { event_bus_ = bus; }

    // Subscribe to MessageReceivedEvent on the event bus
    void subscribe_events();

private:
    Config config_;
    HttpClient& http_;
    std::unordered_map<std::string, Session> sessions_;
    mutable std::mutex mutex_;
    std::string binary_path_;
    EventBus* event_bus_ = nullptr;
    std::unordered_map<std::string, PendingOAuth> pending_oauth_;

    std::optional<PendingOAuth> get_pending_oauth(const std::string& session_id);
    void set_pending_oauth(const std::string& session_id, PendingOAuth pending);
    void clear_pending_oauth(const std::string& session_id);
    void setup_oauth_refresh_callback(Provider* provider);
    bool handle_auth_command(const MessageReceivedEvent& ev,
                             Agent& agent,
                             const std::function<void(const std::string&)>& send_reply);
};

} // namespace ptrclaw
