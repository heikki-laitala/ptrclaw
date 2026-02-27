#include "channels/telegram.hpp"
#include "plugin.hpp"
#include "util.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

static ptrclaw::ChannelRegistrar reg_telegram("telegram",
    [](const ptrclaw::Config& config, ptrclaw::HttpClient& http)
        -> std::unique_ptr<ptrclaw::Channel> {
        auto ch = config.channel_config("telegram");
        if (!ch.contains("bot_token") || ch["bot_token"].get<std::string>().empty()) {
            throw std::runtime_error("Telegram bot_token not configured");
        }
        ptrclaw::TelegramConfig tg_cfg;
        tg_cfg.bot_token = ch["bot_token"].get<std::string>();
        tg_cfg.reply_in_private = ch.value("reply_in_private", true);
        tg_cfg.proxy = ch.value("proxy", std::string{});
        tg_cfg.dev = config.dev;
        tg_cfg.pairing_enabled = ch.value("pairing_enabled", false);
        tg_cfg.pairing_mode = ch.value("pairing_mode", std::string{"auto"});
        tg_cfg.paired_user_id = ch.value("paired_user_id", std::string{});
        tg_cfg.pairing_file = ch.value("pairing_file", std::string{"~/.ptrclaw/telegram_pairing.json"});
        tg_cfg.pairing_admin_chat_id = ch.value("pairing_admin_chat_id", std::string{});
        tg_cfg.pairing_admin_user_id = ch.value("pairing_admin_user_id", std::string{});
        tg_cfg.pairing_pending_file = ch.value("pairing_pending_file", std::string{"~/.ptrclaw/telegram_pairing_pending.json"});
        tg_cfg.pairing_request_ttl_sec = ch.value("pairing_request_ttl_sec", uint64_t(600));
        if (ch.contains("allow_from") && ch["allow_from"].is_array())
            for (const auto& u : ch["allow_from"])
                if (u.is_string()) tg_cfg.allow_from.push_back(u.get<std::string>());
        return std::make_unique<ptrclaw::TelegramChannel>(tg_cfg, http);
    });

namespace ptrclaw {

TelegramChannel::TelegramChannel(const TelegramConfig& config, HttpClient& http)
    : config_(config), http_(http)
{}

void TelegramChannel::initialize() {
    set_my_commands();
    drop_pending_updates();
}

std::string TelegramChannel::api_url(const std::string& method) const {
    return "https://api.telegram.org/bot" + config_.bot_token + "/" + method;
}

bool TelegramChannel::health_check() {
    try {
        auto resp = http_.post(api_url("getMe"), "",
                               {{"Content-Type", "application/json"}}, 10);
        if (resp.status_code != 200) return false;
        auto j = nlohmann::json::parse(resp.body);
        return j.value("ok", false);
    } catch (...) {
        return false;
    }
}

bool TelegramChannel::set_my_commands() {
    nlohmann::json commands = nlohmann::json::array({
        {{"command", "start"}, {"description", "Start conversation"}},
        {{"command", "new"},   {"description", "Clear conversation history"}},
        {{"command", "hatch"}, {"description", "Create or recreate assistant identity"}},
        {{"command", "help"},  {"description", "Show help"}},
    });
    if (config_.dev) {
        commands.push_back({{"command", "soul"}, {"description", "Show current identity"}});
    }
    nlohmann::json body = {{"commands", commands}};

    try {
        auto resp = http_.post(api_url("setMyCommands"), body.dump(),
                               {{"Content-Type", "application/json"}}, 10);
        if (resp.status_code != 200) return false;

        if (config_.pairing_enabled && to_lower(config_.pairing_mode) == "manual" &&
            !config_.pairing_admin_chat_id.empty()) {
            nlohmann::json admin_commands = commands;
            admin_commands.push_back({{"command", "pair"}, {"description", "Pairing admin commands"}});
            nlohmann::json admin_body = {
                {"commands", admin_commands},
                {"scope", {{"type", "chat"}, {"chat_id", std::stoll(config_.pairing_admin_chat_id)}}}
            };
            http_.post(api_url("setMyCommands"), admin_body.dump(),
                       {{"Content-Type", "application/json"}}, 10);
        }

        return true;
    } catch (...) {
        return false;
    }
}

bool TelegramChannel::drop_pending_updates() {
    nlohmann::json body = {{"offset", -1}};
    try {
        auto resp = http_.post(api_url("getUpdates"), body.dump(),
                               {{"Content-Type", "application/json"}}, 10);
        if (resp.status_code != 200) return false;
        auto j = nlohmann::json::parse(resp.body);
        if (j.contains("result") && j["result"].is_array() && !j["result"].empty()) {
            auto& last = j["result"].back();
            if (last.contains("update_id")) {
                last_update_id_ = last["update_id"].get<int64_t>() + 1;
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool TelegramChannel::is_user_allowed(const std::string& username,
                                       const std::vector<std::string>& allow_from) {
    if (allow_from.empty()) return true;

    for (const auto& allowed : allow_from) {
        if (allowed == "*") return true;

        // Case-insensitive comparison, strip leading @ from allowlist entry
        std::string entry = allowed;
        if (!entry.empty() && entry[0] == '@') {
            entry = entry.substr(1);
        }

        // Case-insensitive comparison
        auto to_lower = [](std::string s) {
            std::transform(s.begin(), s.end(), s.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            return s;
        };

        if (to_lower(entry) == to_lower(username)) return true;
    }
    return false;
}

bool TelegramChannel::load_pairing_state() {
    if (pairing_loaded_) return true;
    pairing_loaded_ = true;

    if (!config_.paired_user_id.empty()) return true;

    std::string path = expand_home(config_.pairing_file);
    std::ifstream file(path);
    if (!file.is_open()) return false;

    try {
        auto j = nlohmann::json::parse(file);
        if (j.contains("paired_user_id") && j["paired_user_id"].is_string()) {
            config_.paired_user_id = j["paired_user_id"].get<std::string>();
            return !config_.paired_user_id.empty();
        }
    } catch (const std::exception& e) {
        std::cerr << "[telegram] Warning: failed to load pairing file " << path
                  << ": " << e.what() << "\n";
    }

    return false;
}

bool TelegramChannel::save_pairing_state(const std::string& user_id) {
    if (user_id.empty()) return false;

    nlohmann::json j = {{"paired_user_id", user_id}};
    std::string path = expand_home(config_.pairing_file);
    if (!atomic_write_file(path, j.dump(2) + "\n")) {
        std::cerr << "[telegram] Warning: failed to persist pairing file " << path
                  << " (keeping pairing in memory)\n";
        config_.paired_user_id = user_id;
        pairing_loaded_ = true;
        return false;
    }

    config_.paired_user_id = user_id;
    pairing_loaded_ = true;
    return true;
}

bool TelegramChannel::load_pending_pairing() {
    if (pending_loaded_) return true;
    pending_loaded_ = true;

    std::string path = expand_home(config_.pairing_pending_file);
    std::ifstream file(path);
    if (!file.is_open()) return false;

    try {
        auto j = nlohmann::json::parse(file);
        if (!j.is_object()) return false;
        PendingPairRequest p;
        p.user_id = j.value("user_id", std::string{});
        p.username = j.value("username", std::string{});
        p.first_name = j.value("first_name", std::string{});
        p.chat_id = j.value("chat_id", std::string{});
        p.code = j.value("code", std::string{});
        p.created_at = j.value("created_at", uint64_t(0));
        if (!p.user_id.empty() && !p.code.empty()) pending_pair_ = p;
    } catch (const std::exception& e) {
        std::cerr << "[telegram] Warning: failed to load pending pairing file " << path
                  << ": " << e.what() << "\n";
    }

    return pending_pair_.has_value();
}

bool TelegramChannel::save_pending_pairing() {
    if (!pending_pair_) return false;
    nlohmann::json j = {
        {"user_id", pending_pair_->user_id},
        {"username", pending_pair_->username},
        {"first_name", pending_pair_->first_name},
        {"chat_id", pending_pair_->chat_id},
        {"code", pending_pair_->code},
        {"created_at", pending_pair_->created_at}
    };
    std::string path = expand_home(config_.pairing_pending_file);
    if (!atomic_write_file(path, j.dump(2) + "\n")) {
        std::cerr << "[telegram] Warning: failed to persist pending pairing file " << path << "\n";
        return false;
    }
    return true;
}

bool TelegramChannel::clear_pending_pairing() {
    pending_pair_.reset();
    pending_loaded_ = true;
    std::string path = expand_home(config_.pairing_pending_file);
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return !ec;
}

bool TelegramChannel::pending_expired() const {
    if (!pending_pair_) return true;
    const uint64_t ttl = config_.pairing_request_ttl_sec == 0 ? 600 : config_.pairing_request_ttl_sec;
    return epoch_seconds() > (pending_pair_->created_at + ttl);
}

std::string TelegramChannel::make_pair_code() {
    std::string id = generate_id();
    std::string code;
    for (char c : id) {
        if (std::isalnum(static_cast<unsigned char>(c))) code.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        if (code.size() >= 6) break;
    }
    if (code.size() < 6) code += "PAIR42";
    return code.substr(0, 6);
}

std::string TelegramChannel::normalize_command(const std::string& text) {
    auto s = trim(text);
    auto sp = s.find(' ');
    auto cmd = sp == std::string::npos ? s : s.substr(0, sp);
    auto at = cmd.find('@');
    if (at != std::string::npos) cmd = cmd.substr(0, at);
    return cmd;
}

bool TelegramChannel::is_admin_sender(const std::string& chat_id, const std::string& user_id, bool is_group) const {
    if (is_group) return false;
    bool matched = false;
    if (!config_.pairing_admin_chat_id.empty()) {
        matched = matched || (chat_id == config_.pairing_admin_chat_id);
    }
    if (!config_.pairing_admin_user_id.empty()) {
        matched = matched || (user_id == config_.pairing_admin_user_id);
    }
    return matched;
}

std::optional<std::string> TelegramChannel::admin_target() const {
    if (!config_.pairing_admin_chat_id.empty()) return config_.pairing_admin_chat_id;
    if (!config_.pairing_admin_user_id.empty()) return config_.pairing_admin_user_id;
    return std::nullopt;
}

void TelegramChannel::notify_admin_pair_request() {
    if (!pending_pair_) return;
    auto tgt = admin_target();
    if (!tgt) return;

    std::string who = pending_pair_->username.empty() ? pending_pair_->user_id : ("@" + pending_pair_->username);
    std::ostringstream msg;
    msg << "Pair request from " << who << " (id: " << pending_pair_->user_id << ").\n"
        << "Approve: /pair approve " << pending_pair_->code << "\n"
        << "Deny: /pair deny " << pending_pair_->code << "\n"
        << "Status: /pair status";
    send_message(*tgt, msg.str());
}

std::vector<ChannelMessage> TelegramChannel::poll_updates() {
    std::vector<ChannelMessage> messages;

    nlohmann::json body = {
        {"offset", last_update_id_},
        {"timeout", 30},
        {"allowed_updates", nlohmann::json::array({"message"})}
    };

    HttpResponse resp;
    try {
        resp = http_.post(api_url("getUpdates"), body.dump(),
                          {{"Content-Type", "application/json"}}, 35);
    } catch (...) {
        return messages;
    }

    if (resp.status_code != 200) return messages;

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(resp.body);
    } catch (...) {
        return messages;
    }

    if (!j.value("ok", false) || !j.contains("result") || !j["result"].is_array()) {
        return messages;
    }

    for (const auto& update : j["result"]) {
        if (!update.contains("update_id")) continue;
        int64_t uid = update["update_id"].get<int64_t>();
        if (uid >= last_update_id_) {
            last_update_id_ = uid + 1;
        }

        if (!update.contains("message")) continue;
        const auto& msg = update["message"];

        // Extract sender info
        std::string username;
        std::string user_id_str;
        std::string first_name;
        if (msg.contains("from") && msg["from"].is_object()) {
            const auto& from = msg["from"];
            if (from.contains("username") && from["username"].is_string())
                username = from["username"].get<std::string>();
            if (from.contains("id") && from["id"].is_number())
                user_id_str = std::to_string(from["id"].get<int64_t>());
            if (from.contains("first_name") && from["first_name"].is_string())
                first_name = from["first_name"].get<std::string>();
        }

        // Extract chat info
        std::string chat_id;
        bool is_group = false;
        if (msg.contains("chat") && msg["chat"].is_object()) {
            const auto& chat = msg["chat"];
            if (chat.contains("id") && chat["id"].is_number())
                chat_id = std::to_string(chat["id"].get<int64_t>());
            if (chat.contains("type") && chat["type"].is_string()) {
                std::string chat_type = chat["type"].get<std::string>();
                is_group = (chat_type == "group" || chat_type == "supergroup");
            }
        }

        // Extract text
        std::string text;
        if (msg.contains("text") && msg["text"].is_string()) {
            text = msg["text"].get<std::string>();
        }

        if (text.empty()) continue;

        const bool is_pair_cmd = normalize_command(text) == "/pair";

        if (config_.pairing_enabled && to_lower(config_.pairing_mode) == "manual" && is_pair_cmd) {
            load_pairing_state();
            load_pending_pairing();
            if (pending_expired()) clear_pending_pairing();

            if (!is_admin_sender(chat_id, user_id_str, is_group)) {
                send_message(chat_id, "Pairing commands are admin-only.");
                continue;
            }

            std::istringstream iss(trim(text));
            std::string cmd;
            std::string action;
            std::string code;
            iss >> cmd >> action >> code;
            action = to_lower(action);

            if (action == "approve") {
                if (!pending_pair_) {
                    send_message(chat_id, "No pending pairing request.");
                } else if (code != pending_pair_->code) {
                    send_message(chat_id, "Invalid pairing code.");
                } else {
                    save_pairing_state(pending_pair_->user_id);
                    send_message(chat_id, "Pairing approved for user_id " + pending_pair_->user_id + ".");
                    if (!pending_pair_->chat_id.empty()) {
                        send_message(pending_pair_->chat_id, "✅ Pairing approved. You can now use the bot.");
                    }
                    clear_pending_pairing();
                }
                continue;
            }

            if (action == "deny") {
                if (!pending_pair_) {
                    send_message(chat_id, "No pending pairing request.");
                } else if (code != pending_pair_->code) {
                    send_message(chat_id, "Invalid pairing code.");
                } else {
                    if (!pending_pair_->chat_id.empty()) {
                        send_message(pending_pair_->chat_id, "❌ Pairing denied by admin.");
                    }
                    clear_pending_pairing();
                    send_message(chat_id, "Pairing request denied.");
                }
                continue;
            }

            if (action == "status") {
                std::string status = config_.paired_user_id.empty()
                    ? "No paired user yet."
                    : ("Paired user_id: " + config_.paired_user_id);
                if (pending_pair_) {
                    status += "\nPending: user_id " + pending_pair_->user_id + " code " + pending_pair_->code;
                }
                send_message(chat_id, status);
                continue;
            }

            if (action == "reset") {
                config_.paired_user_id.clear();
                pairing_loaded_ = true;
                std::error_code ec;
                std::filesystem::remove(expand_home(config_.pairing_file), ec);
                clear_pending_pairing();
                send_message(chat_id, "Pairing reset. Next approved request can pair a new user.");
                continue;
            }

            if (action == "whoami") {
                send_message(chat_id, "chat_id=" + chat_id + "\nuser_id=" + user_id_str);
                continue;
            }

            send_message(chat_id,
                "Usage:\n"
                "/pair status\n"
                "/pair approve <CODE>\n"
                "/pair deny <CODE>\n"
                "/pair reset\n"
                "/pair whoami");
            continue;
        }

        // Optional allowlist gate (applies in both normal and pairing mode)
        bool allowed = is_user_allowed(username, config_.allow_from) ||
                       is_user_allowed(user_id_str, config_.allow_from);
        if (!allowed) continue;

        if (config_.pairing_enabled) {
            load_pairing_state();

            if (to_lower(config_.pairing_mode) == "manual") {
                load_pending_pairing();
                if (pending_expired()) clear_pending_pairing();

                if (config_.paired_user_id.empty()) {
                    if (is_group || user_id_str.empty()) continue;

                    bool is_admin = is_admin_sender(chat_id, user_id_str, is_group);
                    if (!is_admin) {
                        bool create_new = !pending_pair_ || pending_pair_->user_id != user_id_str;
                        if (create_new) {
                            pending_pair_ = PendingPairRequest{user_id_str, username, first_name, chat_id, make_pair_code(), epoch_seconds()};
                            save_pending_pairing();
                            notify_admin_pair_request();
                        }
                        send_message(chat_id, "⏳ Pairing request sent to admin. Please wait for approval.");
                    }
                    continue;
                }

                if (user_id_str != config_.paired_user_id) continue;
            } else {
                // Auto mode: first private message pairs automatically
                if (config_.paired_user_id.empty()) {
                    if (is_group || user_id_str.empty()) continue;
                    save_pairing_state(user_id_str);
                }

                if (user_id_str != config_.paired_user_id) continue;
            }
        }

        // Extract message_id for reply-to
        std::optional<int64_t> msg_id;
        if (msg.contains("message_id") && msg["message_id"].is_number()) {
            msg_id = msg["message_id"].get<int64_t>();
        }

        uint64_t ts = 0;
        if (msg.contains("date") && msg["date"].is_number()) {
            ts = static_cast<uint64_t>(msg["date"].get<int64_t>());
        }

        ChannelMessage cm;
        cm.id = generate_id();
        cm.sender = username.empty() ? user_id_str : username;
        cm.content = text;
        cm.channel = "telegram";
        cm.timestamp = ts;
        cm.reply_target = chat_id;
        cm.message_id = msg_id;
        cm.first_name = first_name.empty() ? std::nullopt : std::make_optional(first_name);
        cm.is_group = is_group;

        messages.push_back(std::move(cm));
    }

    return messages;
}

void TelegramChannel::send_typing_indicator(const std::string& target) {
    nlohmann::json body = {
        {"chat_id", target},
        {"action", "typing"}
    };
    try {
        http_.post(api_url("sendChatAction"), body.dump(),
                   {{"Content-Type", "application/json"}}, 10);
    } catch (...) { // NOLINT(bugprone-empty-catch) — best-effort, ignore failures
    }
}

bool TelegramChannel::supports_streaming_display() const {
    return true;
}

int64_t TelegramChannel::send_streaming_placeholder(const std::string& target) {
    nlohmann::json body = {
        {"chat_id", target},
        {"text", "\u2026"}
    };
    try {
        auto resp = http_.post(api_url("sendMessage"), body.dump(),
                               {{"Content-Type", "application/json"}}, 30);
        if (resp.status_code == 200) {
            auto j = nlohmann::json::parse(resp.body);
            if (j.value("ok", false) && j.contains("result")) {
                return j["result"].value("message_id", int64_t(0));
            }
        }
    } catch (...) {
        return 0;
    }
    return 0;
}

void TelegramChannel::edit_message(const std::string& target, int64_t message_id,
                                    const std::string& text) {
    std::string html = markdown_to_telegram_html(text);
    nlohmann::json body = {
        {"chat_id", target},
        {"message_id", message_id},
        {"text", html},
        {"parse_mode", "HTML"}
    };
    auto resp = http_.post(api_url("editMessageText"), body.dump(),
                           {{"Content-Type", "application/json"}}, 30);
    if (resp.status_code != 200) {
        nlohmann::json plain_body = {
            {"chat_id", target},
            {"message_id", message_id},
            {"text", text}
        };
        http_.post(api_url("editMessageText"), plain_body.dump(),
                   {{"Content-Type", "application/json"}}, 30);
    }
}

void TelegramChannel::send_message(const std::string& target, const std::string& message) {
    auto parts = split_message(message, MAX_MESSAGE_LEN);
    if (parts.empty()) parts.push_back(message);

    for (size_t i = 0; i < parts.size(); i++) {
        std::string text = parts[i];
        if (parts.size() > 1 && i < parts.size() - 1) {
            text += "\n\u23EC"; // ⏬ continuation marker
        }

        // Try HTML parse mode first (with Markdown conversion)
        std::string html = markdown_to_telegram_html(text);

        nlohmann::json body = {
            {"chat_id", target},
            {"text", html},
            {"parse_mode", "HTML"}
        };

        auto resp = http_.post(api_url("sendMessage"), body.dump(),
                               {{"Content-Type", "application/json"}}, 30);

        // If HTML fails, fall back to plain text
        if (resp.status_code != 200) {
            nlohmann::json plain_body = {
                {"chat_id", target},
                {"text", text}
            };
            http_.post(api_url("sendMessage"), plain_body.dump(),
                       {{"Content-Type", "application/json"}}, 30);
        }
    }
}

std::string TelegramChannel::markdown_to_telegram_html(const std::string& md) {
    std::ostringstream out;
    size_t i = 0;

    auto html_escape = [](const std::string& s) -> std::string {
        std::string r;
        r.reserve(s.size());
        for (char c : s) {
            switch (c) {
                case '&': r += "&amp;"; break;
                case '<': r += "&lt;"; break;
                case '>': r += "&gt;"; break;
                case '"': r += "&quot;"; break;
                default: r += c;
            }
        }
        return r;
    };

    while (i < md.size()) {
        // Code block: ```...```
        if (i + 2 < md.size() && md[i] == '`' && md[i+1] == '`' && md[i+2] == '`') {
            i += 3;
            // Skip optional language identifier on same line
            while (i < md.size() && md[i] != '\n' && md[i] != '`') i++;
            if (i < md.size() && md[i] == '\n') i++;

            std::string code;
            while (i < md.size()) {
                if (i + 2 < md.size() && md[i] == '`' && md[i+1] == '`' && md[i+2] == '`') {
                    i += 3;
                    break;
                }
                code += md[i++];
            }
            // Remove trailing newline from code block
            if (!code.empty() && code.back() == '\n') code.pop_back();
            out << "<pre>" << html_escape(code) << "</pre>";
            continue;
        }

        // Inline code: `...`
        if (md[i] == '`') {
            i++;
            std::string code;
            while (i < md.size() && md[i] != '`') {
                code += md[i++];
            }
            if (i < md.size()) i++; // skip closing `
            out << "<code>" << html_escape(code) << "</code>";
            continue;
        }

        // Bold: **text**
        if (i + 1 < md.size() && md[i] == '*' && md[i+1] == '*') {
            i += 2;
            std::string bold;
            while (i < md.size()) {
                if (i + 1 < md.size() && md[i] == '*' && md[i+1] == '*') {
                    i += 2;
                    break;
                }
                bold += md[i++];
            }
            out << "<b>" << html_escape(bold) << "</b>";
            continue;
        }

        // Strikethrough: ~~text~~
        if (i + 1 < md.size() && md[i] == '~' && md[i+1] == '~') {
            i += 2;
            std::string strike;
            while (i < md.size()) {
                if (i + 1 < md.size() && md[i] == '~' && md[i+1] == '~') {
                    i += 2;
                    break;
                }
                strike += md[i++];
            }
            out << "<s>" << html_escape(strike) << "</s>";
            continue;
        }

        // Italic: _text_ (only if not inside a word)
        if (md[i] == '_' && (i == 0 || md[i-1] == ' ' || md[i-1] == '\n')) {
            size_t end = md.find('_', i + 1);
            if (end != std::string::npos && end > i + 1) {
                std::string italic = md.substr(i + 1, end - i - 1);
                out << "<i>" << html_escape(italic) << "</i>";
                i = end + 1;
                continue;
            }
        }

        // Link: [text](url)
        if (md[i] == '[') {
            size_t close = md.find(']', i + 1);
            if (close != std::string::npos && close + 1 < md.size() && md[close + 1] == '(') {
                size_t paren_close = md.find(')', close + 2);
                if (paren_close != std::string::npos) {
                    std::string text = md.substr(i + 1, close - i - 1);
                    std::string url = md.substr(close + 2, paren_close - close - 2);
                    out << "<a href=\"" << html_escape(url) << "\">"
                        << html_escape(text) << "</a>";
                    i = paren_close + 1;
                    continue;
                }
            }
        }

        // Header: # Title → <b>Title</b> (at start of line)
        if (md[i] == '#' && (i == 0 || md[i-1] == '\n')) {
            size_t h = i;
            while (h < md.size() && md[h] == '#') h++;
            while (h < md.size() && md[h] == ' ') h++;
            size_t end = md.find('\n', h);
            if (end == std::string::npos) end = md.size();
            std::string title = md.substr(h, end - h);
            out << "<b>" << html_escape(title) << "</b>";
            i = end;
            continue;
        }

        // Bullet list: "- item" at start of line
        if (md[i] == '-' && md.size() > i + 1 && md[i+1] == ' ' &&
            (i == 0 || md[i-1] == '\n')) {
            out << "\u2022"; // •
            i++; // skip the dash, keep the space
            continue;
        }

        // HTML escape for plain characters
        switch (md[i]) {
            case '&': out << "&amp;"; break;
            case '<': out << "&lt;"; break;
            case '>': out << "&gt;"; break;
            default: out << md[i];
        }
        i++;
    }

    return out.str();
}

} // namespace ptrclaw
