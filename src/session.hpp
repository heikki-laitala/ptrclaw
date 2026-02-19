#pragma once
#include "agent.hpp"
#include "config.hpp"
#include "http.hpp"
#include <string>
#include <memory>
#include <unordered_map>
#include <mutex>
#include <cstdint>

namespace ptrclaw {

struct Session {
    std::string id;
    std::unique_ptr<Agent> agent;
    uint64_t last_active = 0;
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

private:
    Config config_;
    HttpClient& http_;
    std::unordered_map<std::string, Session> sessions_;
    mutable std::mutex mutex_;
};

} // namespace ptrclaw
