#pragma once
#include "agent.hpp"
#include "tool_manager.hpp"
#include "config.hpp"
#include "http.hpp"
#ifdef PTRCLAW_HAS_OPENAI_OAUTH
#include "oauth.hpp"
#endif
#include <string>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <optional>
#include <functional>
#include <vector>

namespace ptrclaw {

class EventBus; // forward declaration
struct MessageReceivedEvent; // forward declaration

struct Session {
    std::string id;
    std::unique_ptr<Agent> agent;
    std::unique_ptr<ToolManager> tool_manager;
    uint64_t last_active = 0;
};

class SessionManager {
public:
    static constexpr const char* kCliSessionId = "cli";

    SessionManager(Config& config, HttpClient& http);

    // Get or create a session
    Agent& get_session(const std::string& session_id);

    // Remove a session
    void remove_session(const std::string& session_id);

    // Evict idle sessions (older than max_idle_seconds)
    void evict_idle(uint64_t max_idle_seconds = 3600);

    // Declare a conversation over. The session stops serving immediately and is freed by
    // reap_ended(), which the poll loop calls where freeing one is safe.
    //
    // Safe to call for an id with no live session: a task whose session was already evicted
    // for idleness still has a workspace on disk, and this is what removes it.
    void end_session(const std::string& session_id);

    // Whether end_session() has left anything for reap_ended() to do.
    bool has_pending_end() const;

    // Frees the sessions end_session() marked and deletes their workspaces, returning how
    // many were freed.
    //
    // MUST run with no turn in flight — see the comment in main.cpp's poll loop. An Agent's
    // event handlers are copied out of the bus before being called, so destroying one while
    // a worker is mid-publish is a use-after-free, exactly as for evict_idle().
    size_t reap_ended();

    // List active session IDs
    std::vector<std::string> list_sessions() const;

    // Binary path — propagated to new agents for cron scheduling
    void set_binary_path(const std::string& path) { binary_path_ = path; }

    // Optional event bus — propagated to new agents
    void set_event_bus(EventBus* bus) { event_bus_ = bus; }

    // Shared embedder — propagated to new agents (caller retains ownership)
    void set_embedder(class Embedder* embedder) { embedder_ = embedder; }

    // Subscribe to MessageReceivedEvent on the event bus
    void subscribe_events();

private:
    Config& config_;
    HttpClient& http_;
    std::unordered_map<std::string, Session> sessions_;
    // Ids whose conversation has been declared over, waiting for reap_ended(). Held between
    // the two calls rather than freed on the spot because the request arrives on the channel
    // thread, where another session's turn may be running.
    std::unordered_set<std::string> ending_;
    mutable std::mutex mutex_;
    std::string binary_path_;
    EventBus* event_bus_ = nullptr;
    Embedder* embedder_ = nullptr;

    // Memory backends, owned here rather than by each Agent.
    //
    // Shared isolation: one instance for every session. N instances over one store
    // is not merely wasteful — JsonMemory holds the whole document and rewrites it
    // wholesale, so concurrent turns would lose each other's writes. One instance
    // is safe under a worker pool because every backend locks internally
    // (BaseMemory::mutex_).
    //
    // Session isolation: one instance per session, each over its own file, kept
    // here so eviction can drop it with the session.
    std::shared_ptr<Memory> shared_memory_;
    std::unordered_map<std::string, std::shared_ptr<Memory>> session_memory_;

    // The response cache is a file-backed store with the same problem, so it gets
    // the same treatment: shared instance, or one per session under isolation.
    // Null unless memory.response_cache is on.
    std::shared_ptr<ResponseCache> shared_cache_;
    std::unordered_map<std::string, std::shared_ptr<ResponseCache>> session_cache_;

    // Stores for a session, creating and configuring them on first use.
    // Caller must hold mutex_.
    std::shared_ptr<Memory> memory_for(const std::string& session_id);
    std::shared_ptr<ResponseCache> cache_for(const std::string& session_id);

    // Create a new session with provider, tools, event bus, embedder.
    // Caller must hold mutex_, and must announce the session afterwards — see
    // get_session(), which publishes once the lock is released.
    Session create_session(const std::string& session_id);

    // Bus entry point: runs dispatch_message and turns a thrown exception into a
    // reply, so a failed turn ends rather than leaving the caller waiting out the
    // channel's turn timeout.
    void handle_message(const MessageReceivedEvent& ev);

    // Dispatch a slash command or regular message. Runs on a TurnPool worker, so
    // everything it touches must belong to this session alone.
    void dispatch_message(const MessageReceivedEvent& ev);

    // Dispatch a slash command. Returns true if the message was one and was handled.
    // Only called when commands are permitted for this session — see
    // Config::allow_channel_commands.
    bool handle_command(const MessageReceivedEvent& ev,
                        Agent& agent,
                        const std::function<void(const std::string&)>& send_reply,
                        const std::function<void()>& begin_hatch);

    // Handle /auth commands (API key setting + OAuth if available)
    bool handle_auth_command(const MessageReceivedEvent& ev,
                             Agent& agent,
                             const std::function<void(const std::string&)>& send_reply);

#ifdef PTRCLAW_HAS_OPENAI_OAUTH
    std::unordered_map<std::string, PendingOAuth> pending_oauth_;

    std::optional<PendingOAuth> get_pending_oauth(const std::string& session_id);
    void set_pending_oauth(const std::string& session_id, PendingOAuth pending);
    void clear_pending_oauth(const std::string& session_id);
#endif
};

} // namespace ptrclaw
