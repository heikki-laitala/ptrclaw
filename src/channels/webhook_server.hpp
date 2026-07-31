#pragma once
#include <string>
#include <string_view>
#include <functional>
#include <map>
#include <atomic>
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
    // It runs on the accept thread, so the server accepts no further connection
    // until it returns — see the class comment.
    // The default initializer is load-bearing: existing callers brace-initialise
    // this struct positionally with three fields, and without it every one of them
    // warns under -Wmissing-field-initializers.
    std::function<void(const BodyWriter&)> stream = nullptr;
};

// Minimal single-threaded TCP HTTP server for receiving webhook calls from a
// local reverse proxy. Designed to sit behind nginx/Caddy; not exposed to the
// internet directly. Handles one connection at a time (reverse proxy queues
// concurrent requests). Runs its accept loop in a background thread.
//
// "One connection at a time" is worth re-reading before using
// WebhookResponse::stream: a streamed response occupies the accept thread for its
// whole lifetime, so a long-lived stream blocks every other request until it
// finishes. Fine for one stream per process; not a general-purpose SSE server.
class WebhookServer {
public:
    using Handler = std::function<WebhookResponse(const WebhookRequest&)>;

    // listen_addr: "host:port", e.g. "127.0.0.1:8080"
    // max_body:    maximum POST body size in bytes; larger bodies get 413
    WebhookServer(std::string listen_addr, uint32_t max_body, Handler handler);
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

    int  server_fd_        = -1;
    int  shutdown_pipe_[2] = {-1, -1};
    std::atomic<bool> running_{false};
    std::thread thread_;
};

// Parse "host:port" into host and port.  Returns false if the string is
// malformed or the port is out of range.
bool parse_listen_addr(const std::string& addr, std::string& host, uint16_t& port);

} // namespace ptrclaw
