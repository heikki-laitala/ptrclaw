#include "channels/whatsapp.hpp"
#include "util.hpp"
#include <nlohmann/json.hpp>

namespace ptrclaw {

WhatsAppChannel::WhatsAppChannel(const WhatsAppConfig& config, HttpClient& http)
    : config_(config), http_(http)
{}

std::string WhatsAppChannel::api_url() const {
    return std::string("https://graph.facebook.com/") + API_VERSION + "/" +
           config_.phone_number_id + "/messages";
}

std::string WhatsAppChannel::normalize_phone(const std::string& phone) {
    if (phone.empty()) return phone;
    if (phone[0] == '+') return phone;
    return "+" + phone;
}

bool WhatsAppChannel::is_number_allowed(const std::string& phone,
                                         const std::vector<std::string>& allow_from) {
    if (allow_from.empty()) return true;

    std::string normalized = normalize_phone(phone);
    for (const auto& allowed : allow_from) {
        if (allowed == "*") return true;
        if (normalize_phone(allowed) == normalized) return true;
    }
    return false;
}

void WhatsAppChannel::send_message(const std::string& target, const std::string& message) {
    // Strip leading + for the "to" field (WhatsApp API expects digits only)
    std::string to = target;
    if (!to.empty() && to[0] == '+') {
        to = to.substr(1);
    }

    nlohmann::json body = {
        {"messaging_product", "whatsapp"},
        {"recipient_type", "individual"},
        {"to", to},
        {"type", "text"},
        {"text", {
            {"preview_url", false},
            {"body", message}
        }}
    };

    http_.post(api_url(), body.dump(),
               {{"Content-Type", "application/json"},
                {"Authorization", "Bearer " + config_.access_token}},
               30);
}

std::vector<WhatsAppParsedMessage> WhatsAppChannel::parse_webhook_payload(
        const std::string& payload) const {
    std::vector<WhatsAppParsedMessage> messages;

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(payload);
    } catch (...) {
        return messages;
    }

    if (!j.contains("entry") || !j["entry"].is_array()) return messages;

    for (const auto& entry : j["entry"]) {
        if (!entry.contains("changes") || !entry["changes"].is_array()) continue;

        for (const auto& change : entry["changes"]) {
            if (!change.contains("value") || !change["value"].is_object()) continue;
            const auto& value = change["value"];

            if (!value.contains("messages") || !value["messages"].is_array()) continue;

            for (const auto& msg : value["messages"]) {
                // Only process text messages
                if (!msg.contains("type") || !msg["type"].is_string() ||
                    msg["type"].get<std::string>() != "text") {
                    continue;
                }

                if (!msg.contains("text") || !msg["text"].is_object() ||
                    !msg["text"].contains("body") || !msg["text"]["body"].is_string()) {
                    continue;
                }

                std::string sender;
                if (msg.contains("from") && msg["from"].is_string()) {
                    sender = normalize_phone(msg["from"].get<std::string>());
                }

                if (sender.empty()) continue;

                // Authorization check
                if (!is_number_allowed(sender, config_.allow_from)) continue;

                std::string text = msg["text"]["body"].get<std::string>();

                uint64_t ts = 0;
                if (msg.contains("timestamp") && msg["timestamp"].is_string()) {
                    try {
                        ts = std::stoull(msg["timestamp"].get<std::string>());
                    } catch (...) {
                        ts = epoch_seconds();
                    }
                } else {
                    ts = epoch_seconds();
                }

                messages.push_back(WhatsAppParsedMessage{sender, text, ts});
            }
        }
    }

    return messages;
}

} // namespace ptrclaw
