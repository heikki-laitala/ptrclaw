#include "channels/whatsapp.hpp"
#include "plugin.hpp"
#include "util.hpp"
#include <nlohmann/json.hpp>
#include <chrono>
#include <iostream>

static ptrclaw::ChannelRegistrar reg_whatsapp("whatsapp",
    [](const ptrclaw::Config& config, ptrclaw::HttpClient& http)
        -> std::unique_ptr<ptrclaw::Channel> {
        auto ch = config.channel_config("whatsapp");
        if (!ch.contains("access_token") || ch["access_token"].get<std::string>().empty()) {
            throw std::runtime_error("WhatsApp access_token not configured");
        }
        ptrclaw::WhatsAppConfig wa_cfg;
        wa_cfg.access_token = ch["access_token"].get<std::string>();
        wa_cfg.phone_number_id = ch.value("phone_number_id", std::string{});
        wa_cfg.verify_token = ch.value("verify_token", std::string{});
        wa_cfg.app_secret = ch.value("app_secret", std::string{});
        wa_cfg.webhook_listen = ch.value("webhook_listen", std::string{});
        wa_cfg.webhook_secret = ch.value("webhook_secret", std::string{});
        if (ch.contains("webhook_max_body") && ch["webhook_max_body"].is_number_unsigned())
            wa_cfg.webhook_max_body = ch["webhook_max_body"].get<uint32_t>();
        if (ch.contains("allow_from") && ch["allow_from"].is_array())
            for (const auto& p : ch["allow_from"])
                if (p.is_string()) wa_cfg.allow_from.push_back(p.get<std::string>());
        return std::make_unique<ptrclaw::WhatsAppChannel>(wa_cfg, http);
    });

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

// ── Webhook server mode ───────────────────────────────────────────────────────

bool WhatsAppChannel::supports_polling() const {
    return !config_.webhook_listen.empty();
}

void WhatsAppChannel::initialize() {
    if (config_.webhook_listen.empty()) return;

    server_ = std::make_unique<WebhookServer>(
        config_.webhook_listen,
        config_.webhook_max_body,
        [this](const WebhookRequest& req) -> WebhookResponse {
            return handle_webhook_request(req);
        });

    std::string error;
    if (!server_->start(error)) {
        throw std::runtime_error("WhatsApp webhook server: " + error);
    }
    std::cerr << "[whatsapp] Webhook server listening on " << config_.webhook_listen << "\n";
}

std::vector<ChannelMessage> WhatsAppChannel::poll_updates() {
    std::vector<ChannelMessage> result;
    std::unique_lock<std::mutex> lock(queue_mutex_);
    if (message_queue_.empty()) {
        queue_cv_.wait_for(lock, std::chrono::milliseconds(100));
    }
    result.swap(message_queue_);
    return result;
}

WebhookResponse WhatsAppChannel::handle_webhook_request(const WebhookRequest& req) {
    if (req.path != "/webhook") {
        return {404, "text/plain", "Not Found"};
    }

    if (req.method == "GET") {
        // Meta webhook verification handshake
        if (req.query_param("hub.mode") == "subscribe" &&
            !config_.verify_token.empty() &&
            req.query_param("hub.verify_token") == config_.verify_token) {
            return {200, "text/plain", req.query_param("hub.challenge")};
        }
        return {403, "text/plain", "Forbidden"};
    }

    if (req.method == "POST") {
        // Enforce shared secret when configured (proxy-to-local trust)
        if (!config_.webhook_secret.empty()) {
            auto it = req.headers.find("x-webhook-secret");
            if (it == req.headers.end() || it->second != config_.webhook_secret) {
                return {403, "text/plain", "Forbidden"};
            }
        }

        // Parse payload and push authorised text messages into the queue.
        auto parsed = parse_webhook_payload(req.body);
        if (!parsed.empty()) {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            for (auto& msg : parsed) {
                ChannelMessage cm;
                cm.id          = std::to_string(msg.timestamp) + "_" + msg.sender;
                cm.sender      = msg.sender;
                cm.content     = msg.content;
                cm.channel     = "whatsapp";
                cm.timestamp   = msg.timestamp;
                cm.reply_target = msg.sender;
                message_queue_.push_back(std::move(cm));
            }
            queue_cv_.notify_one();
        }

        return {200, "application/json", R"({"status":"ok"})"};
    }

    return {405, "text/plain", "Method Not Allowed"};
}

// ── Webhook payload parser ────────────────────────────────────────────────────

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
