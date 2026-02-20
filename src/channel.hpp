#pragma once
#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include <optional>

namespace ptrclaw {

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
};

// Abstract base class for messaging channels
class Channel {
public:
    virtual ~Channel() = default;

    virtual std::string channel_name() const = 0;
    virtual bool health_check() = 0;
    virtual void send_message(const std::string& target, const std::string& message) = 0;

    // Channel lifecycle: called once before the poll loop starts
    virtual void initialize() {}

    // Return true if this channel uses polling (vs. webhooks)
    virtual bool supports_polling() const { return false; }

    // Poll for new messages; default returns empty (webhook channels)
    virtual std::vector<ChannelMessage> poll_updates() { return {}; }

    // Streaming display: progressive message editing
    virtual bool supports_streaming_display() const { return false; }
    virtual int64_t send_streaming_placeholder(const std::string& /*target*/) { return 0; }
    virtual void edit_message(const std::string& /*target*/, int64_t /*message_id*/,
                              const std::string& /*text*/) {}

    // Split a message into chunks respecting max_len, preferring newline/space boundaries
    static std::vector<std::string> split_message(const std::string& text, size_t max_len);
};

} // namespace ptrclaw
