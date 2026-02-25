#include "session.hpp"
#include "event.hpp"
#include "event_bus.hpp"
#include "prompt.hpp"
#include "util.hpp"
#include "providers/openai.hpp"

#include <nlohmann/json.hpp>
#ifdef PTRCLAW_USE_COMMONCRYPTO
#include <CommonCrypto/CommonDigest.h>
#else
#include <openssl/sha.h>
#endif
#include <sstream>
#include <iomanip>
#include <fstream>

namespace ptrclaw {

using json = nlohmann::json;

namespace {

std::string url_encode(const std::string& s) {
    std::ostringstream out;
    out << std::hex << std::uppercase;
    for (unsigned char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out << c;
        } else {
            out << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(c);
        }
    }
    return out.str();
}

std::string base64url_encode(const unsigned char* data, size_t len) {
    static const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve((len * 4 + 2) / 3);

    size_t i = 0;
    while (i + 3 <= len) {
        uint32_t n = (static_cast<uint32_t>(data[i]) << 16) |
                     (static_cast<uint32_t>(data[i + 1]) << 8) |
                     static_cast<uint32_t>(data[i + 2]);
        out.push_back(alphabet[(n >> 18) & 63]);
        out.push_back(alphabet[(n >> 12) & 63]);
        out.push_back(alphabet[(n >> 6) & 63]);
        out.push_back(alphabet[n & 63]);
        i += 3;
    }

    if (i < len) {
        uint32_t n = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < len) n |= static_cast<uint32_t>(data[i + 1]) << 8;
        out.push_back(alphabet[(n >> 18) & 63]);
        out.push_back(alphabet[(n >> 12) & 63]);
        if (i + 1 < len) {
            out.push_back(alphabet[(n >> 6) & 63]);
        }
    }

    return out;
}

std::string make_code_verifier() {
    // 32 bytes -> 43 chars in base64url (PKCE-compliant length)
    auto id = generate_id() + generate_id();
    return base64url_encode(reinterpret_cast<const unsigned char*>(id.data()), id.size());
}

std::string make_code_challenge_s256(const std::string& verifier) {
#ifdef PTRCLAW_USE_COMMONCRYPTO
    unsigned char hash[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(verifier.data(), static_cast<CC_LONG>(verifier.size()), hash);
    return base64url_encode(hash, CC_SHA256_DIGEST_LENGTH);
#else
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(verifier.data()), verifier.size(), hash);
    return base64url_encode(hash, SHA256_DIGEST_LENGTH);
#endif
}

std::string query_param(const std::string& input, const std::string& key) {
    auto qpos = input.find('?');
    std::string query = (qpos == std::string::npos) ? input : input.substr(qpos + 1);
    auto hash = query.find('#');
    if (hash != std::string::npos) query = query.substr(0, hash);

    auto parts = split(query, '&');
    for (const auto& p : parts) {
        auto eq = p.find('=');
        if (eq == std::string::npos) continue;
        if (p.substr(0, eq) == key) {
            return p.substr(eq + 1);
        }
    }
    return "";
}

bool persist_openai_oauth(const ProviderEntry& entry) {
    std::string path = expand_home("~/.ptrclaw/config.json");
    json j;
    {
        std::ifstream in(path);
        if (!in.is_open()) return false;
        try { in >> j; } catch (...) { return false; }
    }

    if (!j.contains("providers") || !j["providers"].is_object()) {
        j["providers"] = json::object();
    }
    if (!j["providers"].contains("openai") || !j["providers"]["openai"].is_object()) {
        j["providers"]["openai"] = json::object();
    }

    auto& o = j["providers"]["openai"];
    o["use_oauth"] = entry.use_oauth;
    o["oauth_access_token"] = entry.oauth_access_token;
    o["oauth_refresh_token"] = entry.oauth_refresh_token;
    o["oauth_expires_at"] = entry.oauth_expires_at;
    o["oauth_client_id"] = entry.oauth_client_id;
    o["oauth_token_url"] = entry.oauth_token_url;

    return atomic_write_file(path, j.dump(4) + "\n");
}

struct ParsedOAuthInput {
    std::string code;
    std::string state;
};

ParsedOAuthInput parse_oauth_input(const std::string& raw_input) {
    std::string input = trim(raw_input);
    ParsedOAuthInput result;
    result.code = input;
    result.state = query_param(input, "state");
    std::string code_from_query = query_param(input, "code");
    if (!code_from_query.empty()) result.code = code_from_query;
    return result;
}

// Returns error message (empty on success).
std::string exchange_oauth_token(const std::string& code,
                                  const PendingOAuth& pending,
                                  const ProviderEntry& openai_entry,
                                  HttpClient& http,
                                  ProviderEntry& out_entry) {
    std::string token_url = openai_entry.oauth_token_url.empty()
        ? "https://auth.openai.com/oauth/token"
        : openai_entry.oauth_token_url;
    std::string client_id = openai_entry.oauth_client_id.empty()
        ? "openai-codex"
        : openai_entry.oauth_client_id;

    try {
        json token_req = {
            {"grant_type", "authorization_code"},
            {"code", code},
            {"redirect_uri", pending.redirect_uri},
            {"code_verifier", pending.code_verifier},
            {"client_id", client_id}
        };
        auto resp = http.post(token_url, token_req.dump(),
                              {{"Content-Type", "application/json"}}, 120);

        if (resp.status_code < 200 || resp.status_code >= 300) {
            return "Token exchange failed (HTTP " +
                   std::to_string(resp.status_code) + ").";
        }

        auto tok = json::parse(resp.body);
        std::string access = tok.value("access_token", "");
        if (access.empty()) {
            return "Token exchange succeeded but access_token is missing.";
        }
        std::string refresh = tok.value("refresh_token", "");
        uint64_t expires_in = tok.value("expires_in", 3600u);

        out_entry = openai_entry;
        out_entry.use_oauth = true;
        out_entry.oauth_access_token = access;
        if (!refresh.empty()) out_entry.oauth_refresh_token = refresh;
        out_entry.oauth_expires_at = epoch_seconds() + expires_in;
        if (out_entry.oauth_client_id.empty()) out_entry.oauth_client_id = client_id;
        if (out_entry.oauth_token_url.empty()) out_entry.oauth_token_url = token_url;
    } catch (const std::exception& e) {
        return std::string("OpenAI auth failed: ") + e.what();
    }

    return "";
}

} // namespace

SessionManager::SessionManager(const Config& config, HttpClient& http)
    : config_(config), http_(http)
{}

Agent& SessionManager::get_session(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = sessions_.find(session_id);
    if (it != sessions_.end()) {
        it->second.last_active = epoch_seconds();
        return *(it->second.agent);
    }

    // Create new session
    auto provider_it = config_.providers.find(config_.provider);
    auto provider = create_provider(
        config_.provider,
        config_.api_key_for(config_.provider),
        http_,
        config_.base_url_for(config_.provider),
        config_.prompt_caching_for(config_.provider),
        provider_it != config_.providers.end() ? &provider_it->second : nullptr);
    setup_oauth_refresh_callback(provider.get());

    auto tools = create_builtin_tools();

    Session session;
    session.id = session_id;
    session.agent = std::make_unique<Agent>(std::move(provider), std::move(tools), config_);
    session.last_active = epoch_seconds();

    // Propagate binary path to new agent
    if (!binary_path_.empty()) {
        session.agent->set_binary_path(binary_path_);
    }

    // Propagate event bus to new agent
    if (event_bus_) {
        session.agent->set_event_bus(event_bus_);
        session.agent->set_session_id(session_id);

        SessionCreatedEvent ev;
        ev.session_id = session_id;
        event_bus_->publish(ev);
    }

    auto [inserted, _] = sessions_.emplace(session_id, std::move(session));
    return *(inserted->second.agent);
}

void SessionManager::remove_session(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.erase(session_id);
}

void SessionManager::evict_idle(uint64_t max_idle_seconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t now = epoch_seconds();

    for (auto it = sessions_.begin(); it != sessions_.end(); ) {
        if ((now - it->second.last_active) > max_idle_seconds) {
            if (event_bus_) {
                SessionEvictedEvent ev;
                ev.session_id = it->first;
                event_bus_->publish(ev);
            }
            it = sessions_.erase(it);
        } else {
            ++it;
        }
    }
}

std::vector<std::string> SessionManager::list_sessions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> ids;
    ids.reserve(sessions_.size());
    for (const auto& [id, _] : sessions_) {
        ids.push_back(id);
    }
    return ids;
}

std::optional<PendingOAuth> SessionManager::get_pending_oauth(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pending_oauth_.find(session_id);
    if (it == pending_oauth_.end()) return std::nullopt;
    if (it->second.created_at > 0 &&
        epoch_seconds() - it->second.created_at > kPendingOAuthExpirySeconds) {
        pending_oauth_.erase(it);
        return std::nullopt;
    }
    return it->second;
}

void SessionManager::set_pending_oauth(const std::string& session_id, PendingOAuth pending) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_oauth_[session_id] = std::move(pending);
}

void SessionManager::clear_pending_oauth(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_oauth_.erase(session_id);
}

void SessionManager::setup_oauth_refresh_callback(Provider* provider) {
    auto* oai = dynamic_cast<OpenAIProvider*>(provider);
    if (!oai) return;
    oai->set_on_token_refresh(
        [this](const std::string& at, const std::string& rt, uint64_t ea) {
            auto& entry = config_.providers["openai"];
            entry.oauth_access_token = at;
            entry.oauth_refresh_token = rt;
            entry.oauth_expires_at = ea;
            persist_openai_oauth(entry);
        });
}

void SessionManager::subscribe_events() {
    if (!event_bus_) return;

    ptrclaw::subscribe<MessageReceivedEvent>(*event_bus_,
        [this](const MessageReceivedEvent& ev) {
            auto& agent = get_session(ev.session_id);
            if (!ev.message.channel.empty()) {
                agent.set_channel(ev.message.channel);
            }
            std::string chat_id = ev.message.reply_target.value_or("");

            auto send_reply = [&](const std::string& content) {
                MessageReadyEvent reply;
                reply.session_id = ev.session_id;
                reply.reply_target = chat_id;
                reply.content = content;
                event_bus_->publish(reply);
            };

            auto begin_hatch = [&]() {
                agent.start_hatch();
                send_reply(agent.process(
                    "The user wants to start hatching. Begin the interview."));
            };

            auto apply_oauth_result = [&](const PendingOAuth& pending,
                                          const std::string& code) {
                auto openai_it = config_.providers.find("openai");
                if (openai_it == config_.providers.end()) {
                    send_reply("OpenAI provider config missing.");
                    return;
                }

                ProviderEntry updated;
                std::string err = exchange_oauth_token(
                    code, pending, openai_it->second, http_, updated);
                if (!err.empty()) {
                    send_reply(err);
                    return;
                }

                config_.providers["openai"] = updated;
                bool persisted = persist_openai_oauth(updated);

                auto fresh = create_provider(
                    "openai", config_.api_key_for("openai"), http_,
                    config_.base_url_for("openai"),
                    config_.prompt_caching_for("openai"), &updated);
                setup_oauth_refresh_callback(fresh.get());
                agent.set_provider(std::move(fresh));

                clear_pending_oauth(ev.session_id);

                send_reply(std::string("OpenAI OAuth connected ✅") +
                           (persisted
                            ? " Saved to ~/.ptrclaw/config.json"
                            : " (warning: could not persist to config file)"));
            };

            // Handle /start command
            if (ev.message.content == "/start") {
                if (agent.memory() && !agent.is_hatched()) {
                    begin_hatch();
                } else {
                    std::string greeting = "Hello";
                    if (ev.message.first_name) greeting += " " + *ev.message.first_name;
                    greeting += "! I'm PtrClaw, an AI assistant. How can I help you?";
                    send_reply(greeting);
                }
                return;
            }

            // Handle /new command
            if (ev.message.content == "/new") {
                agent.clear_history();
                send_reply("Conversation cleared. What would you like to discuss?");
                return;
            }

            // Handle /soul command — developer-only
            if (ev.message.content == "/soul") {
                if (!config_.dev) {
                    send_reply("Unknown command: /soul");
                    return;
                }
                std::string display;
                if (agent.memory()) {
                    display = format_soul_display(agent.memory());
                }
                send_reply(display.empty()
                    ? "No soul data yet. Use /hatch to create one."
                    : display);
                return;
            }

            // Handle /hatch command
            if (ev.message.content == "/hatch") {
                begin_hatch();
                return;
            }

            // Handle auth commands
            if (ev.message.content.rfind("/auth", 0) == 0) {
                auto parts = split(ev.message.content, ' ');

                if (parts.size() >= 2 && parts[1] == "status") {
                    auto it = config_.providers.find("openai");
                    if (it == config_.providers.end()) {
                        send_reply("No OpenAI provider config found.");
                        return;
                    }
                    const auto& openai = it->second;
                    std::string status = "OpenAI auth: ";
                    if (openai.use_oauth) {
                        status += "OAuth enabled";
                        if (!openai.oauth_access_token.empty()) {
                            status += " (token present)";
                        }
                        if (openai.oauth_expires_at > 0) {
                            status += "\nExpires at (epoch): " + std::to_string(openai.oauth_expires_at);
                        }
                    } else if (!openai.api_key.empty()) {
                        status += "API key";
                    } else {
                        status += "not configured";
                    }
                    send_reply(status);
                    return;
                }

                if (parts.size() >= 3 && parts[1] == "openai" && parts[2] == "start") {
                    auto openai_it = config_.providers.find("openai");
                    if (openai_it == config_.providers.end()) {
                        send_reply("OpenAI provider config missing.");
                        return;
                    }

                    std::string state = generate_id();
                    std::string verifier = make_code_verifier();
                    std::string challenge = make_code_challenge_s256(verifier);
                    std::string redirect_uri = "http://127.0.0.1:1455/auth/callback";
                    std::string client_id = openai_it->second.oauth_client_id.empty()
                        ? "openai-codex"
                        : openai_it->second.oauth_client_id;

                    PendingOAuth pending;
                    pending.provider = "openai";
                    pending.state = state;
                    pending.code_verifier = verifier;
                    pending.redirect_uri = redirect_uri;
                    pending.created_at = epoch_seconds();
                    set_pending_oauth(ev.session_id, std::move(pending));

                    std::string scope = "openid profile email offline_access";
                    std::string url =
                        "https://auth.openai.com/oauth/authorize"
                        "?response_type=code"
                        "&client_id=" + url_encode(client_id) +
                        "&redirect_uri=" + url_encode(redirect_uri) +
                        "&scope=" + url_encode(scope) +
                        "&state=" + url_encode(state) +
                        "&code_challenge=" + url_encode(challenge) +
                        "&code_challenge_method=S256";

                    send_reply(
                        "Open this URL to authorize OpenAI:\n" + url +
                        "\n\nThen paste the full callback URL with:\n"
                        "/auth openai finish <callback_url>\n"
                        "(or paste just the code)");
                    return;
                }

                if (parts.size() >= 4 && parts[1] == "openai" && parts[2] == "finish") {
                    auto pending = get_pending_oauth(ev.session_id);
                    if (!pending || pending->provider != "openai") {
                        send_reply("No pending OpenAI auth flow. Start with: /auth openai start");
                        return;
                    }

                    std::string input = ev.message.content.substr(
                        ev.message.content.find("finish") + 7);
                    auto parsed = parse_oauth_input(input);

                    if (parsed.code.empty()) {
                        send_reply("Missing code. Paste callback URL or auth code.");
                        return;
                    }
                    if (!parsed.state.empty() && parsed.state != pending->state) {
                        send_reply("State mismatch. Please restart with /auth openai start");
                        return;
                    }

                    apply_oauth_result(*pending, parsed.code);
                    return;
                }

                send_reply("Auth commands:\n/auth status\n/auth openai start\n/auth openai finish <callback_url_or_code>");
                return;
            }

            // If OpenAI OAuth is pending, accept raw callback URL/code directly.
            auto pending_oauth = get_pending_oauth(ev.session_id);
            if (pending_oauth && pending_oauth->provider == "openai") {
                std::string raw = trim(ev.message.content);
                bool looks_like_oauth_reply =
                    (!raw.empty() &&
                     (raw.find("code=") != std::string::npos ||
                      raw.find("auth/callback") != std::string::npos ||
                      raw.find("127.0.0.1:1455") != std::string::npos));

                if (looks_like_oauth_reply && raw.rfind("/auth", 0) != 0) {
                    auto parsed = parse_oauth_input(raw);

                    if (parsed.code.empty()) {
                        send_reply("Missing code. Paste callback URL or auth code.");
                        return;
                    }
                    if (!parsed.state.empty() && parsed.state != pending_oauth->state) {
                        send_reply("State mismatch. Please restart with /auth openai start");
                        return;
                    }

                    apply_oauth_result(*pending_oauth, parsed.code);
                    return;
                }
            }

            // Auto-hatch: if memory exists but no soul, enter hatching
            // so the user's first message kicks off the interview
            if (agent.memory() && !agent.is_hatched() && !agent.hatching()) {
                agent.start_hatch();
            }

            send_reply(agent.process(ev.message.content));
        });
}

} // namespace ptrclaw
