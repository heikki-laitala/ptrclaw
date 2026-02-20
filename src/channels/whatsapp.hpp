#pragma once
#include "channel.hpp"
#include "http.hpp"
#include <string>
#include <vector>

namespace ptrclaw {

struct WhatsAppConfig {
    std::string access_token;
    std::string phone_number_id;
    std::string verify_token;
    std::string app_secret; // optional, for webhook signature verification
    std::vector<std::string> allow_from; // E.164 phone numbers
};

struct WhatsAppParsedMessage {
    std::string sender;  // E.164 format: +1234567890
    std::string content; // text body
    uint64_t timestamp = 0;
};

class WhatsAppChannel : public Channel {
public:
    static constexpr const char* API_VERSION = "v18.0";

    WhatsAppChannel(const WhatsAppConfig& config, HttpClient& http);

    std::string channel_name() const override { return "whatsapp"; }
    bool health_check() override { return true; }
    void send_message(const std::string& target, const std::string& message) override;

    // Parse incoming webhook payload into messages
    std::vector<WhatsAppParsedMessage> parse_webhook_payload(const std::string& payload) const;

    // Get verify token for webhook verification handshake
    const std::string& verify_token() const { return config_.verify_token; }

    // Normalize phone number to E.164 (prepend + if missing)
    static std::string normalize_phone(const std::string& phone);

    // Check if a phone number is in the allowlist
    static bool is_number_allowed(const std::string& phone,
                                  const std::vector<std::string>& allow_from);

    // Build API URL for the WhatsApp Business Cloud API
    std::string api_url() const;

private:
    WhatsAppConfig config_;
    HttpClient& http_;
};

} // namespace ptrclaw
