#pragma once
#include "../channel.hpp"
#include <string>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <unordered_map>

namespace ptrclaw {

// EmbedChannel exposes ptrclaw as an in-process channel for host apps
// (Flutter/Dart FFI, Swift, Kotlin JNI, Python ctypes, etc.).
//
// The host pushes messages via send_user_message() and blocks until the
// response is ready.  Internally, the channel integrates with the standard
// EventBus / SessionManager / StreamRelay pipeline — so commands, memory,
// skills, streaming, and session management all work identically to
// Telegram or WhatsApp.
//
// Thread safety: send_user_message() may be called from any thread.
// The internal poll loop runs on a dedicated background thread.
class EmbedChannel : public Channel {
public:
    EmbedChannel();
    ~EmbedChannel() override;

    // ── Channel interface ────────────────────────────────────────
    std::string channel_name() const override { return "embed"; }
    bool health_check() override { return true; }
    bool supports_polling() const override { return true; }
    void send_message(const std::string& target, const std::string& message) override;
    std::vector<ChannelMessage> poll_updates() override;

    // ── Host API ─────────────────────────────────────────────────

    // Send a message and block until the full response is ready.
    // Returns the assistant's response text.
    std::string send_user_message(const std::string& session_id,
                                  const std::string& message);

private:
    // Inbound message queue (host → channel)
    std::mutex inbound_mutex_;
    std::condition_variable inbound_cv_;
    std::vector<ChannelMessage> inbound_queue_;

    // Per-session response delivery (channel → host)
    struct PendingResponse {
        std::string content;
        bool ready = false;
    };

    std::mutex response_mutex_;
    std::condition_variable response_cv_;
    std::unordered_map<std::string, PendingResponse> pending_responses_;
};

} // namespace ptrclaw
