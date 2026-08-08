#include "channels/http.hpp"

#include <algorithm>
#include "dispatcher.hpp"
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
        // From the serving section rather than the channel block: it belongs with the rest
        // of the multi-session settings, and this channel is the only one a caller names a
        // session on.
        cfg.generate_session_ids = config.serving.generate_session_ids;
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

// Why a pushed window was refused, in a form a caller can branch on.
//
// The sentence is for a human reading a log; `code` is the contract. What a caller needs to
// decide is whether a repaired window could work — "history_unbalanced" says yes, and names
// the call to repair around, while "history_malformed" says the request itself is wrong and
// retrying it is a loop. Matching on the English would have tied callers to wording that
// gets reworded.
struct HistoryError {
    std::string message;
    std::string code;
    std::string tool_call_id;  // empty when no single call is at fault
};

constexpr const char* kHistoryUnbalanced = "history_unbalanced";
constexpr const char* kHistoryMalformed  = "history_malformed";

WebhookResponse json_error(int status, const std::string& message,
                           const std::string& code = "",
                           const std::string& tool_call_id = "") {
    WebhookResponse r;
    r.status = status;
    r.content_type = "application/json";
    json body{{"error", message}};
    // Omitted rather than sent empty: an absent code means unclassified, and a caller
    // switching on it should not have to treat "" as a category.
    if (!code.empty())         body["code"] = code;
    if (!tool_call_id.empty()) body["tool_call_id"] = tool_call_id;
    r.body = body.dump();
    return r;
}

// Parse a pushed history window. Returns false on anything malformed: a caller that meant
// to push context and got it wrong should be told, not silently answered without it.
// Reads the tool_calls an assistant entry carries into the form Agent stores them in: a
// JSON array in the message's `name` field, which is what the providers already read back
// when they replay a turn (anthropic.cpp maps it to tool_use blocks, openai.cpp to a
// tool_calls array). Nothing new is invented here — this is the same encoding agent.cpp
// writes at the end of every tool-calling iteration.
bool parse_tool_calls(const json& raw, std::string& encoded,
                      std::vector<std::string>& ids, HistoryError& error) {
    if (!raw.is_array()) {
        error = {"'tool_calls' must be an array", kHistoryMalformed, ""};
        return false;
    }
    json calls = json::array();
    for (const auto& call : raw) {
        if (!call.is_object() || !call.contains("id") || !call["id"].is_string() ||
            !call.contains("name") || !call["name"].is_string()) {
            error = {"each tool call needs string 'id' and 'name'",
                     kHistoryMalformed, ""};
            return false;
        }
        // Arguments travel as the raw JSON string the model produced, which is how
        // ToolCall holds them — re-encoding an object here would not round-trip a model
        // that emitted something unusual, and the stream hands them out the same way.
        std::string arguments = "{}";
        if (call.contains("arguments")) {
            if (!call["arguments"].is_string()) {
                error = {"tool call 'arguments' must be a string of JSON",
                         kHistoryMalformed, call["id"].get<std::string>()};
                return false;
            }
            arguments = call["arguments"].get<std::string>();
            // Parsed to check, then discarded: the original text is what gets stored, so a
            // model's exact formatting survives the round trip. Unchecked, the failure
            // lands far away and quietly — Anthropic drops the tool_use block whose
            // arguments will not parse but keeps the tool_result that answers it, which is
            // a corrupted replay rather than an error anyone can act on.
            if (!json::accept(arguments)) {
                error = {"tool call '" + call["id"].get<std::string>() +
                         "' has 'arguments' that are not valid JSON",
                         kHistoryMalformed, call["id"].get<std::string>()};
                return false;
            }
        }
        ids.push_back(call["id"].get<std::string>());
        calls.push_back({{"id", call["id"]}, {"name", call["name"]},
                         {"arguments", arguments}});
    }
    if (calls.empty()) {
        error = {"'tool_calls' must not be empty", kHistoryMalformed, ""};
        return false;
    }
    encoded = calls.dump();
    return true;
}

bool parse_history(const json& raw, std::vector<ChatMessage>& out, HistoryError& error) {
    if (!raw.is_array()) {
        error = {"history must be an array", kHistoryMalformed, ""};
        return false;
    }
    // Calls made by the assistant entry immediately preceding the current position, still
    // waiting for their results.
    //
    // Position matters, not just pairing. A provider requires the results to follow the
    // assistant message that made the calls: anything in between, or a call left
    // unanswered, is a 400 upstream. A caller trimming a window to fit a size limit can
    // produce exactly that by dropping whole turns oldest-first, splitting a call from its
    // result — so it is caught here, where the error can name the call that is stranded
    // rather than surfacing as a provider error nobody can act on.
    std::vector<std::string> outstanding;

    for (const auto& entry : raw) {
        if (!entry.is_object() || !entry.contains("role") || !entry.contains("content") ||
            !entry["role"].is_string() || !entry["content"].is_string()) {
            error = {"each history entry needs string 'role' and 'content'",
                     kHistoryMalformed, ""};
            return false;
        }
        const std::string role = entry["role"].get<std::string>();
        const std::string content = entry["content"].get<std::string>();

        // Anything that is not a tool result ends the answering window, so a call still
        // outstanding at that point was never answered where the provider expects it.
        if (role != "tool" && !outstanding.empty()) {
            error = {"tool call '" + outstanding.front() +
                     "' must be answered by a tool result immediately after the assistant "
                     "message that made it",
                     kHistoryUnbalanced, outstanding.front()};
            return false;
        }

        if (role == "system") {
            out.push_back(ChatMessage{Role::System, content, {}, {}});
        } else if (role == "user") {
            out.push_back(ChatMessage{Role::User, content, {}, {}});
        } else if (role == "assistant") {
            std::optional<std::string> encoded;
            if (entry.contains("tool_calls")) {
                std::string calls;
                if (!parse_tool_calls(entry["tool_calls"], calls, outstanding, error)) {
                    return false;
                }
                encoded = calls;
            }
            out.push_back(ChatMessage{Role::Assistant, content, encoded, {}});
        } else if (role == "tool") {
            if (!entry.contains("tool_call_id") || !entry["tool_call_id"].is_string() ||
                entry["tool_call_id"].get<std::string>().empty()) {
                error = {"a 'tool' entry needs a non-empty 'tool_call_id': a result is "
                         "only meaningful paired with the call it answers",
                         kHistoryMalformed, ""};
                return false;
            }
            const std::string id = entry["tool_call_id"].get<std::string>();
            auto waiting = std::find(outstanding.begin(), outstanding.end(), id);
            if (waiting == outstanding.end()) {
                error = {"tool result '" + id + "' answers no preceding tool call",
                         kHistoryUnbalanced, id};
                return false;
            }
            outstanding.erase(waiting);
            // `name` carries the tool name, matching format_tool_result_message().
            std::optional<std::string> name;
            if (entry.contains("name") && entry["name"].is_string()) {
                name = entry["name"].get<std::string>();
            }
            out.push_back(ChatMessage{Role::Tool, content, name, id});
        } else {
            error = {"unknown role '" + role + "'", kHistoryMalformed, ""};
            return false;
        }
    }

    if (!outstanding.empty()) {
        error = {"tool call '" + outstanding.front() + "' has no result in this window",
                 kHistoryUnbalanced, outstanding.front()};
        return false;
    }
    return true;
}

} // namespace

HttpChannel::HttpChannel(HttpChannelConfig config) : config_(std::move(config)) {}

HttpChannel::~HttpChannel() {
    // Order matters. WebhookServer::stop() waits for its connection threads, and each of
    // those may be parked in stream_turn() waiting for a reply the poll loop will now never
    // produce — its own writer-based cancellation only fires once a write is attempted, and
    // a parked thread attempts none. So shutdown would stall for the full turn timeout,
    // 120 s by default, long past any termination grace period. Release the waiters first.
    // Guarded because a destructor must not throw, and both steps allocate or lock:
    // release_pending_turns() copies its reason into each turn, and stop() joins threads.
    // Nothing here is recoverable — if it fails, waiters fall back to their own turn
    // timeout — so swallowing is the honest behaviour rather than terminating.
    try {
        release_pending_turns("server is shutting down");
        if (server_) server_->stop();
    } catch (...) {  // NOLINT(bugprone-empty-catch) — see above
    }
}

void HttpChannel::release_pending_turns(const std::string& reason) {
    std::vector<TurnRef> pending;
    {
        std::lock_guard<std::mutex> lk(turn_mutex_);
        pending.reserve(turns_.size());
        for (auto& [session, turn] : turns_) {
            (void)session;
            if (!turn->done) {
                turn->error = reason;
                turn->done = true;
            }
            pending.push_back(turn);
        }
    }
    // Every waiter, but one notify each on its own cv rather than a broadcast to all of
    // them per turn. Outside the lock: the waiters need it to make progress.
    for (const auto& turn : pending) turn->cv.notify_one();
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
    // Tool activity goes out on the same stream as the tokens, in order.
    //
    // Without it a turn is only observable as text, so a caller that owns the transcript
    // cannot record what the agent actually did — and a window pushed back reconstructs
    // the conversation as though no tool had run. Both frames carry the tool_call_id,
    // which is what makes the exchange pushable back through `history`.
    //
    // New event types are additive for SSE: a client subscribes to the names it knows, so
    // one reading `token` and `done` is unaffected by these.
    subscribe<ToolCallRequestEvent>(*bus_, [this](const ToolCallRequestEvent& ev) {
        enqueue_frame(ev.session_id, sse_frame("tool_call", json{
            {"id", ev.tool_call_id},
            {"name", ev.tool_name},
            // Which assistant message this call belongs to. The agent publishes a batch's
            // calls one at a time and results arrive as they finish, so a fast result can
            // land before the next call is announced — without this a transcript owner
            // cannot tell one interleaved batch from two successive rounds, and the
            // `tool_calls` array it has to rebuild groups exactly one batch.
            {"batch", ev.batch_id},
            // The raw JSON string the model produced, not a re-encoded object: a caller
            // pushing it back must be able to hand over exactly what was sent.
            {"arguments", ev.arguments_json},
        }));
    });

    subscribe<ToolCallResultEvent>(*bus_, [this](const ToolCallResultEvent& ev) {
        // First result per call wins, matching what the agent keeps. A tool cancelled for
        // exceeding the timeout can still finish and publish afterwards; history already
        // holds the synthesised timeout by then, so emitting the late one would hand the
        // caller a transcript the agent never had.
        if (!claim_tool_result(ev.session_id, ev.tool_call_id)) return;
        enqueue_frame(ev.session_id, sse_frame("tool_result", json{
            {"id", ev.tool_call_id},
            {"name", ev.tool_name},
            {"batch", ev.batch_id},
            {"success", ev.success},
            // Exactly what goes into the agent's own history, prefix and all. Exporting the
            // raw output instead would let a caller replay content the model never saw —
            // and, for a failure, drop the only signal that the tool failed at all.
            {"output", tool_result_content(ev.success, ev.output)},
        }));
    });

    // What the turn cost, and how much of it the provider served from cache. The pod goes
    // to some trouble to keep its prompt prefix byte-stable so that cache can hit; without
    // this the effect is invisible from outside and a regression shows up as a bill.
    subscribe<ProviderResponseEvent>(*bus_, [this](const ProviderResponseEvent& ev) {
        std::lock_guard<std::mutex> lk(turn_mutex_);
        auto it = turns_.find(ev.session_id);
        if (it == turns_.end()) return;  // no turn of ours to attribute it to
        it->second->usage.prompt_tokens += ev.usage.prompt_tokens;
        it->second->usage.completion_tokens += ev.usage.completion_tokens;
        it->second->usage.total_tokens += ev.usage.total_tokens;
        it->second->usage.cached_prompt_tokens += ev.usage.cached_prompt_tokens;
    });

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
    std::unique_lock<std::mutex> lk(inbound_mutex_);
    // Blocks briefly when there is nothing to do, matching WhatsAppChannel. main.cpp's
    // poll loop has no sleep of its own, so returning immediately spins it: an idle
    // deployment would burn a core and re-run session eviction continuously. The wait is
    // bounded rather than indefinite so shutdown still takes effect within 100 ms.
    if (inbound_queue_.empty()) {
        inbound_cv_.wait_for(lk, std::chrono::milliseconds(100),
                             [this] { return !inbound_queue_.empty(); });
    }
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

    const bool ending = req.path == "/session/end";
    if (!ending && req.path != "/chat") return json_error(404, "not found");
    if (req.method != "POST")           return json_error(405, "method not allowed");

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

    if (ending) {
        // The id is always required here. generate_session_ids exists so a new conversation
        // need not name itself; there is no ending a session nobody named, and inventing an
        // id to delete would be the one place where guessing costs someone their files.
        if (!body.contains("session") || !body["session"].is_string())
            return json_error(400, "'session' is required and must be a string");
        const std::string session = body["session"].get<std::string>();
        if (session.empty()) return json_error(400, "'session' must not be empty");

        // Whatever the session was doing, it is over. Clearing the guard here means a
        // caller whose turn was abandoned — the client hung up, and nothing else would
        // ever clear the entry — can end the session rather than wait out the timeout.
        {
            std::lock_guard<std::mutex> lk(turn_mutex_);
            auto it = turns_.find(session);
            if (it != turns_.end()) {
                // Whoever is parked in stream_turn() re-checks only when notified. Without
                // this it would sleep out the whole turn timeout on a turn that no longer
                // exists, holding a connection slot for a task already cancelled.
                it->second->detached = true;
                it->second->cv.notify_one();
                turns_.erase(it);
            }
        }

        if (bus_) {
            SessionEndRequestedEvent ev;
            ev.session_id = session;
            bus_->publish(ev);
        }

        // 202, not 200: SessionManager frees the session once no turn is in flight anywhere
        // in the pod, which is a poll iteration away. Accepted is the honest answer, and it
        // is the same answer for an id the pod has never seen — the caller asked for it to
        // be gone, and it is.
        WebhookResponse r;
        r.status = 202;
        r.content_type = "application/json";
        r.body = json{{"session", session}, {"status", "ending"}}.dump();
        return r;
    }

    // A caller starting a new conversation has nothing to name it yet, so an absent, null
    // or empty session may be filled in — but only where that was asked for. Everywhere
    // else the id stays required, byte-for-byte as before.
    const bool session_omitted =
        !body.contains("session") || body["session"].is_null() ||
        (body["session"].is_string() && body["session"].get<std::string>().empty());
    const bool generate = config_.generate_session_ids && session_omitted;

    // is_string() before extracting, not value<std::string>(): nlohmann throws on a type
    // mismatch rather than returning the default, so {"session": 1} would escape to the
    // handler wrapper and become a 500 for what is plainly a malformed request.
    if (!generate && (!body.contains("session") || !body["session"].is_string()))
        return json_error(400, "'session' is required and must be a string");
    if (!body.contains("message") || !body["message"].is_string())
        return json_error(400, "'message' is required and must be a string");

    // secure_random_hex(), not generate_id(): under the serving profile a session id
    // selects a private workspace and memory store, so it is a capability rather than a
    // routing key, and mt19937's state is recoverable from its own output.
    const std::string session = generate
        ? secure_random_hex(16)
        : body["session"].get<std::string>();
    const std::string message = body["message"].get<std::string>();
    if (session.empty()) return json_error(400, "'session' must not be empty");
    if (message.empty()) return json_error(400, "'message' must not be empty");

    ChannelMessage msg;
    msg.sender = session;        // SessionManager keys sessions off sender
    msg.reply_target = session;  // and send_message() routes the reply back by it
    msg.content = message;
    msg.channel = "http";
    msg.timestamp = epoch_seconds();

    if (body.contains("history")) {
        std::vector<ChatMessage> window;
        HistoryError error;
        if (!parse_history(body["history"], window, error)) {
            return json_error(400, error.message, error.code, error.tool_call_id);
        }
        msg.history = std::move(window);
    }

    // One in-flight turn per session, and the alternative is worse than it looks.
    // Everything downstream keys off the session: the reply arrives via send_message(target
    // = session), and the deltas via StreamChunkEvent(session_id). Two overlapping turns
    // would therefore share one mailbox — both streams racing for the first reply, one
    // closing empty, the second reply discarded. Keying by a request id would make the
    // routing unambiguous but still leave two turns interleaving over one Agent and one
    // history, which is not a conversation.
    //
    // Registered before the message is queued: the poll thread can pick it up and start
    // publishing deltas the moment it is visible, and a delta arriving with no Turn to
    // hold it would be dropped.
    TurnRef turn;
    {
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lk(turn_mutex_);
        auto existing = turns_.find(session);
        if (existing != turns_.end()) {
            // Unless it is abandoned: WebhookServer returns without invoking the producer
            // if the response headers fail to send, and nothing else would ever clear that
            // entry. Without this the session would be wedged for the process's lifetime.
            const bool stale = now - existing->second->started >
                               std::chrono::seconds(config_.turn_timeout_seconds);
            if (!stale) {
                return json_error(409, "a turn is already in flight for this session");
            }
            // Detach before dropping it: a thread may still be parked on the stale turn,
            // and it has to learn the turn is gone rather than wait out its own deadline.
            existing->second->detached = true;
            existing->second->cv.notify_one();
            turns_.erase(existing);
        }
        turn = std::make_shared<Turn>();
        turn->started = now;
        turn->seq = next_turn_seq_++;
        turns_.emplace(session, turn);
    }
    {
        std::lock_guard<std::mutex> lk(inbound_mutex_);
        inbound_queue_.push_back(std::move(msg));
    }
    inbound_cv_.notify_one();  // so the poll loop reacts now, not up to 100 ms later

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
    resp.stream = [this, session_ref, generate, turn](const BodyWriter& write) noexcept {
        try {
            // Before any token, and only when the id was invented: the caller cannot
            // continue the conversation without it, and a browser EventSource client
            // cannot read response headers. A caller that supplied its own id sees the
            // stream it has always seen.
            if (generate) {
                // A false return means the client is already gone. Waiting for a reply it
                // cannot receive would hold this connection thread for the whole turn
                // timeout, so repeated early disconnects would exhaust max_connections;
                // the turn is released here instead of waiting to go stale.
                if (!write(sse_frame("session", json{{"session", *session_ref}}))) {
                    std::lock_guard<std::mutex> lk(turn_mutex_);
                    erase_turn(*session_ref, turn->seq);
                    return;
                }
            }
            stream_turn(*session_ref, turn, write);
        } catch (...) {
            // Nothing useful can be reported from here, and the client's stream is
            // already lost. Dropping the turn is what matters: the reply then has
            // nowhere to be delivered instead of accumulating unread.
            std::lock_guard<std::mutex> lk(turn_mutex_);
            erase_turn(*session_ref, turn->seq);
        }
    };
    return resp;
}

void HttpChannel::erase_turn(const std::string& session, uint64_t seq) {
    auto it = turns_.find(session);
    // A different seq means this turn was already taken away and the id reused: erasing
    // would drop somebody else's live turn.
    if (it == turns_.end() || it->second->seq != seq) return;
    it->second->detached = true;
    it->second->cv.notify_one();
    turns_.erase(it);
}

void HttpChannel::stream_turn(const std::string& session, const TurnRef& turn,
                              const BodyWriter& write) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(config_.turn_timeout_seconds);

    for (;;) {
        std::string frame;
        bool last = false;

        {
            std::unique_lock<std::mutex> lk(turn_mutex_);
            // Waits on this turn's own cv, so only the connection that owns it is woken.
            // The turn is held by shared_ptr, so it stays alive even once erased from the
            // map — which is what makes `detached` readable rather than a dangling read.
            const bool ready = turn->cv.wait_until(lk, deadline, [&] {
                return turn->detached || !turn->pending.empty() || turn->done;
            });

            if (!ready) {
                erase_turn(session, turn->seq);
                lk.unlock();
                write(sse_frame("error", json{{"message", "turn timed out"}}));
                return;
            }

            // Taken away: ended by the caller, or replaced by a newer turn under the same
            // id after being abandoned. Either way this stream has nothing left to report,
            // and anything now queued under that id belongs to a different client.
            if (turn->detached) return;

            if (!turn->pending.empty()) {
                frame = turn->pending.front();
                turn->pending.pop_front();
            } else if (!turn->error.empty()) {
                frame = sse_frame("error", json{{"message", turn->error}});
                last = true;
                erase_turn(session, turn->seq);
            } else {
                frame = sse_frame("done", json{
                    {"content", turn->final_content},
                    // Summed over the turn's provider calls, this session's only.
                    {"usage", {
                        {"prompt_tokens", turn->usage.prompt_tokens},
                        {"completion_tokens", turn->usage.completion_tokens},
                        {"total_tokens", turn->usage.total_tokens},
                        {"cached_prompt_tokens", turn->usage.cached_prompt_tokens},
                    }},
                });
                last = true;
                erase_turn(session, turn->seq);
            }
        }

        // Written outside the lock on purpose: a send blocks until the client reads, and
        // holding turn_mutex_ across it would stall the poll thread — i.e. one slow reader
        // would stop every other conversation in the process.
        if (!write(frame)) {
            // Peer gone, or the server is shutting down. Drop the turn so the reply has
            // nowhere to be delivered and cannot accumulate.
            std::lock_guard<std::mutex> lk(turn_mutex_);
            erase_turn(session, turn->seq);
            return;
        }
        if (last) return;
    }
}

void HttpChannel::append_delta(const std::string& session, const std::string& delta) {
    enqueue_frame(session, sse_frame("token", json{{"delta", delta}}));
}

bool HttpChannel::claim_tool_result(const std::string& session, const std::string& id) {
    std::lock_guard<std::mutex> lk(turn_mutex_);
    auto it = turns_.find(session);
    if (it == turns_.end()) return false;  // no turn to report into
    return it->second->reported_results.insert(id).second;
}

void HttpChannel::enqueue_frame(const std::string& session, const std::string& frame) {
    TurnRef turn;
    {
        std::lock_guard<std::mutex> lk(turn_mutex_);
        auto it = turns_.find(session);
        if (it == turns_.end()) return;  // not ours, or already finished
        it->second->pending.push_back(frame);
        turn = it->second;
    }
    // One waiter, not every connection in the process. Outside the lock so the woken
    // thread does not immediately block on the mutex we are still holding.
    turn->cv.notify_one();
}

void HttpChannel::fail_turn(const std::string& session, const std::string& error) {
    TurnRef turn;
    {
        std::lock_guard<std::mutex> lk(turn_mutex_);
        auto it = turns_.find(session);
        if (it == turns_.end()) return;
        it->second->error = error;
        it->second->done = true;
        turn = it->second;
    }
    turn->cv.notify_one();
}

void HttpChannel::send_message(const std::string& target, const std::string& message) {
    // The reply arrives here from StreamRelay's MessageReadyEvent handler, on the thread
    // that ran the turn. It is the authoritative end of a turn — StreamEndEvent is not,
    // because a non-streaming provider produces no stream events at all and only this
    // fires.
    TurnRef turn;
    {
        std::lock_guard<std::mutex> lk(turn_mutex_);
        auto it = turns_.find(target);
        if (it == turns_.end()) return;  // client already gone
        it->second->final_content = message;
        it->second->done = true;
        turn = it->second;
    }
    turn->cv.notify_one();
}

} // namespace ptrclaw
