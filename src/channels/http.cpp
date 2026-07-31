#include "channels/http.hpp"
#include "event.hpp"
#include "event_bus.hpp"
#include "plugin.hpp"
#include "util.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <memory>
#include <iostream>
#include <stdexcept>

using json = nlohmann::json;

static ptrclaw::ChannelRegistrar reg_http("http",
    [](const ptrclaw::Config& config, ptrclaw::HttpClient& /*http*/)
        -> std::unique_ptr<ptrclaw::Channel> {
        auto ch = config.channel_config("http");
        ptrclaw::HttpChannelConfig cfg;
        cfg.listen = ch.value("listen", cfg.listen);
        cfg.secret = ch.value("secret", std::string{});
        if (ch.contains("max_body") && ch["max_body"].is_number_unsigned())
            cfg.max_body = ch["max_body"].get<uint32_t>();
        if (ch.contains("max_connections") && ch["max_connections"].is_number_unsigned())
            cfg.max_connections = ch["max_connections"].get<uint32_t>();
        if (ch.contains("turn_timeout_seconds") &&
            ch["turn_timeout_seconds"].is_number_unsigned())
            cfg.turn_timeout_seconds = ch["turn_timeout_seconds"].get<uint32_t>();
        return std::make_unique<ptrclaw::HttpChannel>(cfg);
    });

namespace ptrclaw {

namespace {

// An SSE frame. JSON payloads carry no raw newline (nlohmann escapes them), which is what
// keeps a single data: line valid — a raw newline inside the payload would terminate the
// frame early and silently corrupt the stream.
std::string sse_frame(const char* event, const json& data) {
    return std::string("event: ") + event + "\ndata: " + data.dump() + "\n\n";
}

WebhookResponse json_error(int status, const std::string& message) {
    WebhookResponse r;
    r.status = status;
    r.content_type = "application/json";
    r.body = json{{"error", message}}.dump();
    return r;
}

// Parse a pushed history window. Returns false on anything malformed: a caller that meant
// to push context and got it wrong should be told, not silently answered without it.
bool parse_history(const json& raw, std::vector<ChatMessage>& out, std::string& error) {
    if (!raw.is_array()) {
        error = "history must be an array";
        return false;
    }
    for (const auto& entry : raw) {
        if (!entry.is_object() || !entry.contains("role") || !entry.contains("content") ||
            !entry["role"].is_string() || !entry["content"].is_string()) {
            error = "each history entry needs string 'role' and 'content'";
            return false;
        }
        const std::string role = entry["role"].get<std::string>();
        Role parsed{};
        if      (role == "system")    parsed = Role::System;
        else if (role == "user")      parsed = Role::User;
        else if (role == "assistant") parsed = Role::Assistant;
        else if (role == "tool")      parsed = Role::Tool;
        else {
            error = "unknown role '" + role + "'";
            return false;
        }
        out.push_back(ChatMessage{parsed, entry["content"].get<std::string>(), {}, {}});
    }
    return true;
}

} // namespace

HttpChannel::HttpChannel(HttpChannelConfig config) : config_(std::move(config)) {}

HttpChannel::~HttpChannel() {
    if (server_) server_->stop();
}

bool HttpChannel::health_check() {
    std::string host;
    uint16_t port = 0;
    if (!parse_listen_addr(config_.listen, host, port)) {
        std::cerr << "[http] invalid listen address: " << config_.listen << "\n";
        return false;
    }
    return true;
}

bool HttpChannel::supports_polling() const {
    return !config_.listen.empty();
}

void HttpChannel::set_event_bus(EventBus* bus) {
    bus_ = bus;
    if (!bus_) return;

    // Subscribed directly rather than through StreamRelay, which is gated on
    // supports_streaming_display() and throttles deltas into progressive edit_message()
    // calls — Telegram's edit-in-place model. SSE wants every token, unthrottled, in order.
    subscribe<StreamChunkEvent>(*bus_, [this](const StreamChunkEvent& ev) {
        append_delta(ev.session_id, ev.delta);
    });
}

void HttpChannel::initialize() {
    server_ = std::make_unique<WebhookServer>(
        config_.listen, config_.max_body,
        [this](const WebhookRequest& req) -> WebhookResponse {
            try {
                return handle_request(req);
            } catch (const std::exception& e) {
                std::cerr << "[http] request failed: " << e.what() << "\n";
                return json_error(500, "internal error");
            }
        },
        config_.max_connections);

    std::string error;
    if (!server_->start(error)) {
        // Fatal: a chat channel that cannot listen has nothing to do, and a silent
        // failure here would look like a channel that simply never receives traffic.
        throw std::runtime_error("http channel failed to listen on " + config_.listen +
                                 ": " + error);
    }
    std::cerr << "[http] listening on " << config_.listen
              << " (max_connections=" << config_.max_connections << ")\n";
}

std::vector<ChannelMessage> HttpChannel::poll_updates() {
    std::vector<ChannelMessage> out;
    std::lock_guard<std::mutex> lk(inbound_mutex_);
    out.swap(inbound_queue_);
    return out;
}

WebhookResponse HttpChannel::handle_request(const WebhookRequest& req) {
    if (req.method == "GET" && req.path == "/healthz") {
        WebhookResponse r;
        r.content_type = "text/plain";
        r.body = "ok";
        return r;
    }

    if (req.path != "/chat")   return json_error(404, "not found");
    if (req.method != "POST")  return json_error(405, "method not allowed");

    if (!config_.secret.empty()) {
        auto it = req.headers.find("authorization");
        const std::string expected = "Bearer " + config_.secret;
        if (it == req.headers.end() || it->second != expected) {
            return json_error(403, "forbidden");
        }
    }

    json body;
    try {
        body = json::parse(req.body);
    } catch (const std::exception&) {
        return json_error(400, "body is not valid JSON");
    }
    if (!body.is_object()) return json_error(400, "body must be a JSON object");

    const std::string session = body.value("session", std::string{});
    const std::string message = body.value("message", std::string{});
    if (session.empty()) return json_error(400, "'session' is required");
    if (message.empty()) return json_error(400, "'message' is required");

    ChannelMessage msg;
    msg.sender = session;        // SessionManager keys sessions off sender
    msg.reply_target = session;  // and send_message() routes the reply back by it
    msg.content = message;
    msg.channel = "http";
    msg.timestamp = epoch_seconds();

    if (body.contains("history")) {
        std::vector<ChatMessage> window;
        std::string error;
        if (!parse_history(body["history"], window, error)) {
            return json_error(400, error);
        }
        msg.history = std::move(window);
    }

    // Registered before the message is queued: the poll thread can pick it up and start
    // publishing deltas the moment it is visible, and a delta arriving with no Turn to
    // hold it would be dropped.
    {
        std::lock_guard<std::mutex> lk(turn_mutex_);
        turns_[session] = Turn{};
    }
    {
        std::lock_guard<std::mutex> lk(inbound_mutex_);
        inbound_queue_.push_back(std::move(msg));
    }

    WebhookResponse resp;
    resp.status = 200;
    resp.content_type = "text/event-stream";
    // Both boundaries into this channel are guarded, because WebhookServer calls the
    // handler and the producer directly on its connection thread: an escaping exception
    // would unwind out of that thread and terminate the process. A closed stream serves
    // the client better than a dead pod.
    // The session is captured through a shared_ptr rather than by value so the closure's
    // copy constructor is noexcept: copying a std::string can throw, and this callback is
    // invoked from a bare connection thread where a throw terminates the process.
    auto session_ref = std::make_shared<const std::string>(session);
    resp.stream = [this, session_ref](const BodyWriter& write) noexcept {
        try {
            stream_turn(*session_ref, write);
        } catch (...) {
            // Nothing useful can be reported from here, and the client's stream is
            // already lost. Dropping the turn is what matters: the reply then has
            // nowhere to be delivered instead of accumulating unread.
            std::lock_guard<std::mutex> lk(turn_mutex_);
            turns_.erase(*session_ref);
        }
    };
    return resp;
}

void HttpChannel::stream_turn(const std::string& session, const BodyWriter& write) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(config_.turn_timeout_seconds);

    for (;;) {
        std::string frame;
        bool last = false;

        {
            std::unique_lock<std::mutex> lk(turn_mutex_);
            const bool ready = turn_cv_.wait_until(lk, deadline, [&] {
                auto it = turns_.find(session);
                return it == turns_.end() || !it->second.deltas.empty() || it->second.done;
            });

            if (!ready) {
                turns_.erase(session);
                lk.unlock();
                write(sse_frame("error", json{{"message", "turn timed out"}}));
                return;
            }

            auto it = turns_.find(session);
            if (it == turns_.end()) return;  // abandoned elsewhere; nothing to report

            if (!it->second.deltas.empty()) {
                frame = sse_frame("token", json{{"delta", it->second.deltas.front()}});
                it->second.deltas.pop_front();
            } else if (!it->second.error.empty()) {
                frame = sse_frame("error", json{{"message", it->second.error}});
                last = true;
                turns_.erase(it);
            } else {
                frame = sse_frame("done", json{{"content", it->second.final_content}});
                last = true;
                turns_.erase(it);
            }
        }

        // Written outside the lock on purpose: a send blocks until the client reads, and
        // holding turn_mutex_ across it would stall the poll thread — i.e. one slow reader
        // would stop every other conversation in the process.
        if (!write(frame)) {
            // Peer gone, or the server is shutting down. Drop the turn so the reply has
            // nowhere to be delivered and cannot accumulate.
            std::lock_guard<std::mutex> lk(turn_mutex_);
            turns_.erase(session);
            return;
        }
        if (last) return;
    }
}

void HttpChannel::append_delta(const std::string& session, const std::string& delta) {
    {
        std::lock_guard<std::mutex> lk(turn_mutex_);
        auto it = turns_.find(session);
        if (it == turns_.end()) return;  // not ours, or already finished
        it->second.deltas.push_back(delta);
    }
    turn_cv_.notify_all();
}

void HttpChannel::fail_turn(const std::string& session, const std::string& error) {
    {
        std::lock_guard<std::mutex> lk(turn_mutex_);
        auto it = turns_.find(session);
        if (it == turns_.end()) return;
        it->second.error = error;
        it->second.done = true;
    }
    turn_cv_.notify_all();
}

void HttpChannel::send_message(const std::string& target, const std::string& message) {
    // The reply arrives here from StreamRelay's MessageReadyEvent handler, on the poll
    // thread. It is the authoritative end of a turn — StreamEndEvent is not, because a
    // non-streaming provider produces no stream events at all and only this fires.
    {
        std::lock_guard<std::mutex> lk(turn_mutex_);
        auto it = turns_.find(target);
        if (it == turns_.end()) return;  // client already gone
        it->second.final_content = message;
        it->second.done = true;
    }
    turn_cv_.notify_all();
}

} // namespace ptrclaw
