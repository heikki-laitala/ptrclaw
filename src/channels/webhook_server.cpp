#include "channels/webhook_server.hpp"
#include "util.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <sstream>
#include <stdexcept>

// MSG_NOSIGNAL prevents SIGPIPE on Linux; macOS uses SO_NOSIGPIPE per-socket.
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace ptrclaw {

// ── URL helpers ───────────────────────────────────────────────────────────────

static std::string url_decode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            char hex[3] = {s[i + 1], s[i + 2], '\0'};
            char* end;
            long val = std::strtol(hex, &end, 16);
            if (end == hex + 2) {
                out += static_cast<char>(val);
                i += 2;
                continue;
            }
        } else if (s[i] == '+') {
            out += ' ';
            continue;
        }
        out += s[i];
    }
    return out;
}

static std::map<std::string, std::string> parse_query_string(const std::string& qs) {
    std::map<std::string, std::string> result;
    size_t start = 0;
    while (start <= qs.size()) {
        size_t amp = qs.find('&', start);
        if (amp == std::string::npos) amp = qs.size();
        std::string pair = qs.substr(start, amp - start);
        auto eq = pair.find('=');
        if (eq != std::string::npos) {
            result[url_decode(pair.substr(0, eq))] = url_decode(pair.substr(eq + 1));
        } else if (!pair.empty()) {
            result[url_decode(pair)] = "";
        }
        start = amp + 1;
    }
    return result;
}

std::string WebhookRequest::query_param(const std::string& key) const {
    auto it = query_params.find(key);
    return it != query_params.end() ? it->second : "";
}

// ── Address parsing ───────────────────────────────────────────────────────────

bool parse_listen_addr(const std::string& addr, std::string& host, uint16_t& port) {
    auto pos = addr.rfind(':');
    if (pos == std::string::npos || pos == 0) return false;
    host = addr.substr(0, pos);
    if (host.empty()) return false;
    try {
        int p = std::stoi(addr.substr(pos + 1));
        if (p <= 0 || p > 65535) return false;
        port = static_cast<uint16_t>(p);
    } catch (...) {
        return false;
    }
    return true;
}

// ── WebhookServer ─────────────────────────────────────────────────────

WebhookServer::WebhookServer(std::string listen_addr,
                                             uint32_t max_body,
                                             Handler handler)
    : listen_addr_(std::move(listen_addr))
    , max_body_(max_body)
    , handler_(std::move(handler))
{}

WebhookServer::~WebhookServer() {
    stop();
}

bool WebhookServer::start(std::string& error) {
    std::string host;
    uint16_t port;
    if (!parse_listen_addr(listen_addr_, host, port)) {
        error = "Invalid listen address: " + listen_addr_;
        return false;
    }

    if (::pipe(shutdown_pipe_) != 0) {
        error = "Failed to create shutdown pipe";
        return false;
    }

    server_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        error = "Failed to create server socket";
        ::close(shutdown_pipe_[0]);
        ::close(shutdown_pipe_[1]);
        return false;
    }

    int opt = 1;
    ::setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

#ifdef SO_NOSIGPIPE  // macOS
    ::setsockopt(server_fd_, SOL_SOCKET, SO_NOSIGPIPE, &opt, sizeof(opt));
#endif

    struct sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &sa.sin_addr) != 1) {
        error = "Invalid bind address: " + host;
        ::close(server_fd_); server_fd_ = -1;
        ::close(shutdown_pipe_[0]); ::close(shutdown_pipe_[1]);
        return false;
    }

    if (::bind(server_fd_, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0) {
        error = std::string("bind failed: ") + std::strerror(errno);
        ::close(server_fd_); server_fd_ = -1;
        ::close(shutdown_pipe_[0]); ::close(shutdown_pipe_[1]);
        return false;
    }

    if (::listen(server_fd_, 16) != 0) {
        error = "listen failed";
        ::close(server_fd_); server_fd_ = -1;
        ::close(shutdown_pipe_[0]); ::close(shutdown_pipe_[1]);
        return false;
    }

    running_.store(true);
    thread_ = std::thread([this]() { accept_loop(); });
    return true;
}

void WebhookServer::stop() {
    if (!running_.exchange(false)) return;
    char b = 0;
    if (shutdown_pipe_[1] >= 0) ::write(shutdown_pipe_[1], &b, 1);
    if (thread_.joinable()) thread_.join();
    if (server_fd_ >= 0)         { ::close(server_fd_);         server_fd_ = -1; }
    if (shutdown_pipe_[0] >= 0)  { ::close(shutdown_pipe_[0]);  shutdown_pipe_[0] = -1; }
    if (shutdown_pipe_[1] >= 0)  { ::close(shutdown_pipe_[1]);  shutdown_pipe_[1] = -1; }
}

void WebhookServer::accept_loop() {
    while (running_.load()) {
        struct pollfd fds[2];
        fds[0].fd = server_fd_;         fds[0].events = POLLIN;
        fds[1].fd = shutdown_pipe_[0];  fds[1].events = POLLIN;

        int ret = ::poll(fds, 2, 1000);
        if (ret <= 0) continue;              // timeout or transient error
        if (fds[1].revents & POLLIN) break;  // shutdown signal
        if (!(fds[0].revents & POLLIN)) continue;

        struct sockaddr_in peer{};
        socklen_t plen = sizeof(peer);
        int cfd = ::accept(server_fd_, reinterpret_cast<sockaddr*>(&peer), &plen);
        if (cfd >= 0) {
            struct timeval tv{10, 0};  // 10s recv timeout
            ::setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            handle_connection(cfd);
            ::close(cfd);
        }
    }
}

// ── HTTP helpers ──────────────────────────────────────────────────────────────

static void send_http_response(int fd, int status, const std::string& content_type,
                               const std::string& body) {
    const char* reason = "OK";
    if      (status == 400) reason = "Bad Request";
    else if (status == 403) reason = "Forbidden";
    else if (status == 404) reason = "Not Found";
    else if (status == 405) reason = "Method Not Allowed";
    else if (status == 413) reason = "Payload Too Large";

    std::string resp =
        "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n"
        "Content-Type: " + content_type + "\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "Connection: close\r\n\r\n" + body;

    ::send(fd, resp.c_str(), resp.size(), MSG_NOSIGNAL);
}

void WebhookServer::handle_connection(int fd) const {
    // Read until end-of-headers (CRLFCRLF), cap at 16 KB.
    std::string buf;
    buf.reserve(4096);
    char tmp[512];

    while (buf.find("\r\n\r\n") == std::string::npos) {
        ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) return;
        buf.append(tmp, static_cast<size_t>(n));
        if (buf.size() > 16384) {
            send_http_response(fd, 400, "text/plain", "Headers too large");
            return;
        }
    }

    auto hdr_end  = buf.find("\r\n\r\n");
    std::string headers_raw = buf.substr(0, hdr_end);
    std::string leftover    = buf.substr(hdr_end + 4);

    // Parse request line.
    auto rl_end = headers_raw.find("\r\n");
    if (rl_end == std::string::npos) return;

    WebhookRequest req;
    {
        std::istringstream ss(headers_raw.substr(0, rl_end));
        std::string pq, ver;
        if (!(ss >> req.method >> pq >> ver)) return;
        auto q = pq.find('?');
        if (q != std::string::npos) {
            req.path         = pq.substr(0, q);
            req.query_params = parse_query_string(pq.substr(q + 1));
        } else {
            req.path = pq;
        }
    }

    // Parse headers.
    size_t pos = rl_end + 2;
    while (pos < headers_raw.size()) {
        auto ne = headers_raw.find("\r\n", pos);
        if (ne == std::string::npos) ne = headers_raw.size();
        std::string hline = headers_raw.substr(pos, ne - pos);
        pos = ne + 2;
        auto col = hline.find(':');
        if (col == std::string::npos) continue;
        req.headers[to_lower(trim(hline.substr(0, col)))] = trim(hline.substr(col + 1));
    }

    // Read body for POST.
    if (req.method == "POST") {
        size_t content_len = 0;
        auto it = req.headers.find("content-length");
        if (it != req.headers.end()) {
            try { content_len = std::stoul(it->second); } catch (...) {}
        }

        if (content_len > max_body_) {
            send_http_response(fd, 413, "text/plain", "Payload too large");
            return;
        }

        req.body = std::move(leftover);
        while (req.body.size() < content_len) {
            ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
            if (n <= 0) break;
            req.body.append(tmp, static_cast<size_t>(n));
            if (req.body.size() > max_body_) {
                send_http_response(fd, 413, "text/plain", "Payload too large");
                return;
            }
        }
    }

    WebhookResponse resp = handler_(req);
    send_http_response(fd, resp.status, resp.content_type, resp.body);
}

} // namespace ptrclaw
