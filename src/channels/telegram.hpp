#pragma once
#include "../channel.hpp"
#include "../http.hpp"
#include <string>
#include <vector>

namespace ptrclaw {

struct TelegramConfig {
    std::string bot_token;
    std::vector<std::string> allow_from;
    bool reply_in_private = true;
    std::string proxy; // optional proxy URL
};

class TelegramChannel : public Channel {
public:
    static constexpr size_t MAX_MESSAGE_LEN = 4096;

    TelegramChannel(const TelegramConfig& config, HttpClient& http);

    std::string channel_name() const override { return "telegram"; }
    bool health_check() override;
    void send_message(const std::string& target, const std::string& message) override;

    void initialize() override;
    bool supports_polling() const override { return true; }
    std::vector<ChannelMessage> poll_updates() override;

    bool supports_streaming_display() const override;
    int64_t send_streaming_placeholder(const std::string& target) override;
    void edit_message(const std::string& target, int64_t message_id,
                      const std::string& text) override;

    // Set bot commands in the Telegram menu
    bool set_my_commands();

    // Drop pending updates (skip offline-accumulated messages)
    bool drop_pending_updates();

    // Check if a user is allowed (case-insensitive, wildcard "*")
    static bool is_user_allowed(const std::string& username,
                                const std::vector<std::string>& allow_from);

    // Convert Markdown to Telegram HTML subset
    static std::string markdown_to_telegram_html(const std::string& md);

    // Build Telegram API URL for a method
    std::string api_url(const std::string& method) const;

    int64_t last_update_id() const { return last_update_id_; }

private:
    TelegramConfig config_;
    HttpClient& http_;
    int64_t last_update_id_ = 0;
};

} // namespace ptrclaw
