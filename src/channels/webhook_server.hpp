#pragma once
#include <string>
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

struct WebhookResponse {
    int         status       = 200;
    std::string content_type = "text/plain";
    std::string body;
};

// Minimal single-threaded TCP HTTP server for receiving webhook calls from a
// local reverse proxy. Designed to sit behind nginx/Caddy; not exposed to the
// internet directly. Handles one connection at a time (reverse proxy queues
// concurrent requests). Runs its accept loop in a background thread.
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
