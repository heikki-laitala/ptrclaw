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

    // Split a message into chunks respecting max_len, preferring newline/space boundaries
    static std::vector<std::string> split_message(const std::string& text, size_t max_len);
};

// Registry of active channels
class ChannelRegistry {
public:
    void register_channel(std::unique_ptr<Channel> ch);
    Channel* find_by_name(const std::string& name) const;
    std::vector<std::string> channel_names() const;
    size_t size() const { return channels_.size(); }

private:
    std::vector<std::unique_ptr<Channel>> channels_;
};

} // namespace ptrclaw
