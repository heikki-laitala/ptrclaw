#pragma once
#include "../channel.hpp"
#include "../http.hpp"
#include <optional>
#include <string>
#include <vector>

namespace ptrclaw {

struct TelegramConfig {
    std::string bot_token;
    std::vector<std::string> allow_from;
    bool reply_in_private = true;
    std::string proxy; // optional proxy URL
    bool dev = false;  // expose developer-only commands in Telegram menu

    // Pairing mode: bind bot usage to a single Telegram user_id
    bool pairing_enabled = false;
    std::string pairing_mode = "auto"; // auto|manual
    std::string paired_user_id;
    std::string pairing_file = "~/.ptrclaw/telegram_pairing.json";

    // Manual pairing controls
    std::string pairing_admin_chat_id;
    std::string pairing_admin_user_id;
    std::string pairing_pending_file = "~/.ptrclaw/telegram_pairing_pending.json";
    uint64_t pairing_request_ttl_sec = 600;
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

    void send_typing_indicator(const std::string& target) override;

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
    std::string paired_user_id() const { return config_.paired_user_id; }

private:
    struct PendingPairRequest {
        std::string user_id;
        std::string username;
        std::string first_name;
        std::string chat_id;
        std::string code;
        uint64_t created_at = 0;
    };

    bool load_pairing_state();
    bool save_pairing_state(const std::string& user_id);
    bool load_pending_pairing();
    bool save_pending_pairing();
    bool clear_pending_pairing();
    bool pending_expired() const;

    static std::string make_pair_code();
    static std::string normalize_command(const std::string& text);
    bool is_admin_sender(const std::string& chat_id, const std::string& user_id, bool is_group) const;
    std::optional<std::string> admin_target() const;
    void notify_admin_pair_request();

    TelegramConfig config_;
    HttpClient& http_;
    int64_t last_update_id_ = 0;
    bool pairing_loaded_ = false;
    bool pending_loaded_ = false;
    std::optional<PendingPairRequest> pending_pair_;
};

} // namespace ptrclaw
