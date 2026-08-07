#pragma once
#include <string>
#include <string_view>
#include <functional>
#include <map>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <cstdint>

namespace ptrclaw {

// A parsed inbound HTTP request from the reverse proxy.
struct WebhookRequest {
    std::string method;   // "GET" or "POST"
    std::string path;     // e.g. "/webhook"
    std::map<std::string, std::string> query_params;  // URL-decoded query parameters
    std::map<std::string, std::string> headers;        // header names lowercased
    std::string body;

    // Return a query parameter value, or "" if absent.
    std::string query_param(const std::string& key) const;
};

// Writes one piece of a streamed response body. Returns false once the peer is
// gone, so a producer can stop early instead of generating output nobody reads.
using BodyWriter = std::function<bool(std::string_view chunk)>;

struct WebhookResponse {
    int         status       = 200;
    std::string content_type = "text/plain";
    std::string body;

    // Set this to stream a body whose length is not known when the handler
    // returns — a token stream, or any text/event-stream response. It is called
    // once, after the headers are sent, and may keep writing for as long as it
    // likes. When set, `body` is ignored.
    //
    // The response is delimited by connection close: no Content-Length and no
    // chunked framing, which is what this server's unconditional
    // "Connection: close" already implies for every response it sends.
    //
    // With the default max_connections of 1 it runs on the accept thread, so the
    // server accepts no further connection until it returns. Above 1 it runs on its
    // own connection thread — see the constructor.
    //
    // The writer also returns false once the server is stopping, so a producer that
    // checks it will unblock shutdown rather than holding it open.
    // The default initializer is load-bearing: existing callers brace-initialise
    // this struct positionally with three fields, and without it every one of them
    // warns under -Wmissing-field-initializers.
    std::function<void(const BodyWriter&)> stream = nullptr;
};

// Minimal TCP HTTP server for receiving webhook calls from a local reverse proxy.
// Designed to sit behind nginx/Caddy; not exposed to the internet directly. Runs its
// accept loop in a background thread.
//
// By default it handles one connection at a time, inline on that thread, and a reverse
// proxy queues the rest. `max_connections` raises that: see the constructor, and note
// that above 1 the handler must be thread-safe.
//
// The distinction matters most for WebhookResponse::stream, because a streamed response
// occupies its handler for the whole reply rather than for a few milliseconds.
// Depth of the kernel's accept queue: connections that have completed the handshake and are
// waiting for the acceptor to take them.
//
// It has to track max_connections rather than sit at a constant. The acceptor deliberately
// leaves a connection over capacity in this queue instead of accepting it to answer 503 —
// so the queue *is* the waiting room, and a fixed 16 capped a pod at 16 waiters however
// high max_connections was set. Past the queue the kernel drops the handshake and the
// client sees a reset rather than an answer.
//
// The kernel caps this itself (SOMAXCONN, net.core.somaxconn), so a large value asks for
// what the system allows rather than promising it.
int listen_backlog(uint32_t max_connections);

class WebhookServer {
public:
    using Handler = std::function<WebhookResponse(const WebhookRequest&)>;

    // listen_addr:     "host:port", e.g. "127.0.0.1:8080"
    // max_body:        maximum POST body size in bytes; larger bodies get 413
    // max_connections: how many connections may be in flight at once.
    //
    // The default of 1 is the original behaviour exactly — connections are handled
    // inline on the accept thread, one after another, and a reverse proxy queues the
    // rest. Nothing changes for a caller that omits it.
    //
    // Above 1, each connection is handled on its own thread, which matters for
    // streamed responses: a stream occupies its handler for the whole reply, so with
    // one slot a second client waits in the kernel accept queue and the 17th is
    // refused outright (`listen` backlog). Refusal is a much worse failure than
    // waiting.
    //
    // ⚠ With max_connections > 1 the handler is called from several threads at once
    // and must be thread-safe. Capacity is checked *before* accepting, so excess
    // connections stay in the kernel backlog and are picked up as slots free rather
    // than being accepted and rejected.
    WebhookServer(std::string listen_addr, uint32_t max_body, Handler handler,
                  uint32_t max_connections = 1);
    ~WebhookServer();

    // Start background accept thread. Returns false and populates error on failure.
    bool start(std::string& error);

    // Signal the accept thread to stop and join it.
    void stop();

private:
    void accept_loop();
    void handle_connection(int client_fd) const;

    std::string listen_addr_;
    uint32_t    max_body_;
    Handler     handler_;
    uint32_t    max_connections_ = 1;

    int  server_fd_        = -1;
    int  shutdown_pipe_[2] = {-1, -1};
    std::atomic<bool> running_{false};
    std::thread thread_;

    // Connection accounting for max_connections_ > 1. Threads are detached, so stop()
    // must wait for this to reach zero before the server is destroyed — a detached
    // thread still holds `this` and calls handler_.
    mutable std::mutex              conn_mutex_;
    mutable std::condition_variable conn_cv_;
    uint32_t                        active_conns_ = 0;
};

// Parse "host:port" into host and port.  Returns false if the string is
// malformed or the port is out of range.
bool parse_listen_addr(const std::string& addr, std::string& host, uint16_t& port);

} // namespace ptrclaw
