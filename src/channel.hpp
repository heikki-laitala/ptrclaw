#pragma once
#include "provider.hpp"  // ChatMessage
#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include <optional>

namespace ptrclaw {

class EventBus;

struct ChannelMessage {
    std::string id;
    std::string sender;
    std::string content;
    std::string channel;
    uint64_t timestamp = 0;
    std::optional<std::string> reply_target;
    std::optional<int64_t> message_id;
    std::optional<std::string> first_name;
    bool is_group = false;

    // Conversation window for this turn, for channels whose frontend owns the
    // history rather than the agent. When set it *replaces* the session's
    // accumulated history before the message is processed, so the agent stays a
    // stateless consumer of whatever the caller sends. A leading System message
    // becomes the system prompt (see Agent::set_history). Unset — the normal
    // case, and every built-in channel — means "use the agent's own history".
    std::optional<std::vector<ChatMessage>> history;
};

// Abstract base class for messaging channels
class Channel {
public:
    virtual ~Channel() = default;

    virtual std::string channel_name() const = 0;
    virtual bool health_check() = 0;
    virtual void send_message(const std::string& target, const std::string& message) = 0;

    // Wired once, before initialize(), so a channel may subscribe there.
    //
    // Only needed to observe agent events directly — emitting tokens as they arrive by
    // subscribing to StreamChunkEvent, for instance. The supports_streaming_display()
    // path below covers progressive message editing and needs no bus.
    virtual void set_event_bus(EventBus* /*bus*/) {}

    // Channel lifecycle: called once before the poll loop starts
    virtual void initialize() {}

    // Return true if this channel uses polling (vs. webhooks)
    virtual bool supports_polling() const { return false; }

    // Poll for new messages; default returns empty (webhook channels)
    virtual std::vector<ChannelMessage> poll_updates() { return {}; }

    // Typing indicator (e.g. "typing..." in Telegram)
    virtual void send_typing_indicator(const std::string& /*target*/) {}

    // Streaming display: progressive message editing
    virtual bool supports_streaming_display() const { return false; }
    virtual int64_t send_streaming_placeholder(const std::string& /*target*/) { return 0; }
    virtual void edit_message(const std::string& /*target*/, int64_t /*message_id*/,
                              const std::string& /*text*/) {}

    // Split a message into chunks respecting max_len, preferring newline/space boundaries
    static std::vector<std::string> split_message(const std::string& text, size_t max_len);
};

} // namespace ptrclaw
