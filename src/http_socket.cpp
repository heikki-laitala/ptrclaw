// Linux HTTP/HTTPS client using POSIX sockets + OpenSSL.
// Implements the same public API as http.cpp (libcurl) with identical
// interface behaviour: http_init/cleanup are no-ops (OpenSSL 1.1+ auto-inits).
#ifdef __linux__

#include "http.hpp"

#include <openssl/ssl.h>
#include <openssl/err.h>

#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>

#include <cstring>
#include <cerrno>
#include <string>
#include <stdexcept>

namespace ptrclaw {

static const std::atomic<bool>* g_socket_abort_flag = nullptr;

void http_init() {}
void http_cleanup() {}

void http_set_abort_flag(const std::atomic<bool>* flag) {
    g_socket_abort_flag = flag;
}

// ── URL parsing ────────────────────────────────────────────────

struct ParsedUrl {
    bool tls;
    std::string host;
    std::string port;
    std::string path; // includes leading / and query string
};

static ParsedUrl parse_url(const std::string& url) {
    ParsedUrl result{};
    size_t scheme_end = url.find("://");
    if (scheme_end == std::string::npos)
        throw std::runtime_error("http_socket: invalid URL: " + url);

    std::string scheme = url.substr(0, scheme_end);
    result.tls = (scheme == "https");

    size_t host_start = scheme_end + 3;
    size_t path_start = url.find('/', host_start);
    std::string host_port = (path_start == std::string::npos)
        ? url.substr(host_start)
        : url.substr(host_start, path_start - host_start);

    result.path = (path_start == std::string::npos) ? "/" : url.substr(path_start);

    size_t colon = host_port.find(':');
    if (colon != std::string::npos) {
        result.host = host_port.substr(0, colon);
        result.port = host_port.substr(colon + 1);
    } else {
        result.host = host_port;
        result.port = result.tls ? "443" : "80";
    }
    return result;
}

// ── RAII connection (TCP + optional TLS) ──────────────────────

struct Connection {
    int      fd  = -1;
    SSL_CTX* ctx = nullptr;
    SSL*     ssl = nullptr;

    Connection() = default;
    ~Connection() {
        if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); }
        if (ctx) SSL_CTX_free(ctx);
        if (fd >= 0) ::close(fd);
    }
    Connection(const Connection&)            = delete;
    Connection& operator=(const Connection&) = delete;

    bool connect(const ParsedUrl& url, long timeout_secs) {
        struct addrinfo hints{};
        hints.ai_family   = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        struct addrinfo* res = nullptr;
        if (getaddrinfo(url.host.c_str(), url.port.c_str(), &hints, &res) != 0)
            return false;

        bool connected = false;
        for (auto* ai = res; ai && !connected; ai = ai->ai_next) {
            fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (fd < 0) continue;

            // Non-blocking connect so we can honour timeout_secs.
            int flags = fcntl(fd, F_GETFL, 0);
            fcntl(fd, F_SETFL, flags | O_NONBLOCK);

            int rc = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
            if (rc == 0) {
                fcntl(fd, F_SETFL, flags);
                connected = true;
            } else if (errno == EINPROGRESS) {
                fd_set wset;
                FD_ZERO(&wset);
                FD_SET(fd, &wset);
                struct timeval tv{timeout_secs, 0};
                rc = select(fd + 1, nullptr, &wset, nullptr, &tv);
                if (rc > 0) {
                    int err = 0;
                    socklen_t elen = sizeof(err);
                    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen);
                    if (err == 0) {
                        fcntl(fd, F_SETFL, flags);
                        connected = true;
                    }
                }
            }
            if (!connected) { ::close(fd); fd = -1; }
        }
        freeaddrinfo(res);
        if (!connected) return false;

        // Use full timeout for TLS handshake, then switch to 1-second slices
        // so abort-flag checks work during body streaming.
        if (url.tls) {
            set_socket_timeout(timeout_secs);

            ctx = SSL_CTX_new(TLS_client_method());
            if (!ctx) return false;
            SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
            SSL_CTX_set_default_verify_paths(ctx);
            SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

            ssl = SSL_new(ctx);
            if (!ssl) return false;
            SSL_set_fd(ssl, fd);
            SSL_set_tlsext_host_name(ssl, url.host.c_str()); // SNI

            if (SSL_connect(ssl) != 1) return false;
        }

        // 1-second slice timeout for body I/O (enables abort-flag polling).
        set_socket_timeout(1);
        return true;
    }

    // Read some bytes; returns >0 on data, 0 on EOF, -1 on unrecoverable error.
    // EAGAIN (1-second slice expiry) loops back so the caller can check abort.
    ssize_t read_some(char* buf, size_t len) {
        while (true) {
            if (g_socket_abort_flag &&
                g_socket_abort_flag->load(std::memory_order_relaxed))
                return -1;

            ssize_t n;
            if (ssl) {
                n = SSL_read(ssl, buf, static_cast<int>(len));
                if (n > 0) return n;
                if (n == 0) return 0;
                int err = SSL_get_error(ssl, static_cast<int>(n));
                if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
                    continue;
                if (err == SSL_ERROR_SYSCALL &&
                    (errno == EAGAIN || errno == EWOULDBLOCK))
                    continue; // 1-second slice expired
                return -1;
            } else {
                n = ::recv(fd, buf, len, 0);
                if (n > 0) return n;
                if (n == 0) return 0;
                if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                return -1;
            }
        }
    }

    bool write_all(const char* buf, size_t len) {
        while (len > 0) {
            ssize_t n;
            if (ssl) {
                n = SSL_write(ssl, buf, static_cast<int>(len));
                if (n <= 0) {
                    int err = SSL_get_error(ssl, static_cast<int>(n));
                    if (err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ)
                        continue;
                    return false;
                }
            } else {
                n = ::send(fd, buf, len, 0);
                if (n < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                    return false;
                }
            }
            buf += n;
            len -= static_cast<size_t>(n);
        }
        return true;
    }

private:
    void set_socket_timeout(long secs) {
        struct timeval tv{secs, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }
};

// ── Request building ───────────────────────────────────────────

static std::string build_request(const std::string& method,
                                  const ParsedUrl& url,
                                  const std::string& body,
                                  const std::vector<Header>& headers) {
    std::string req;
    req.reserve(512 + body.size());
    req += method + " " + url.path + " HTTP/1.1\r\n";
    req += "Host: " + url.host + "\r\n";

    bool has_content_length = false;
    for (const auto& h : headers) {
        req += h.first + ": " + h.second + "\r\n";
        if (h.first == "Content-Length") has_content_length = true;
    }
    if (!body.empty() && !has_content_length)
        req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    req += "Connection: close\r\n\r\n";
    req += body;
    return req;
}

// ── Response parsing ───────────────────────────────────────────

// Read a CRLF-terminated line, using leftover as a look-ahead buffer.
static std::string read_line(Connection& conn, std::string& leftover) {
    while (true) {
        size_t pos = leftover.find('\n');
        if (pos != std::string::npos) {
            std::string line = leftover.substr(0, pos);
            leftover.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return line;
        }
        char buf[4096];
        ssize_t n = conn.read_some(buf, sizeof(buf));
        if (n <= 0) return "";
        leftover.append(buf, static_cast<size_t>(n));
    }
}

// Parse status line + headers; populates is_chunked / content_length.
static long parse_response_headers(Connection& conn, std::string& leftover,
                                    bool& is_chunked, size_t& content_length) {
    is_chunked     = false;
    content_length = 0;

    std::string status_line = read_line(conn, leftover);
    if (status_line.empty()) return 0;

    // "HTTP/1.1 200 OK" — extract the three-digit code
    size_t sp1 = status_line.find(' ');
    if (sp1 == std::string::npos) return 0;
    long status = 0;
    try { status = std::stol(status_line.substr(sp1 + 1, 3)); }
    catch (...) { return 0; }

    while (true) {
        std::string line = read_line(conn, leftover);
        if (line.empty()) break; // blank line → end of headers

        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;

        std::string name  = line.substr(0, colon);
        std::string value = line.substr(colon + 1);
        while (!value.empty() && (value[0] == ' ' || value[0] == '\t'))
            value.erase(0, 1);

        for (auto& c : name)  c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        for (auto& c : value) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));

        if (name == "transfer-encoding")
            is_chunked = (value.find("chunked") != std::string::npos);
        else if (name == "content-length")
            try { content_length = std::stoul(value); } catch (...) {}
    }
    return status;
}

// Read exactly n bytes, consuming leftover first.
static bool read_exactly(Connection& conn, std::string& leftover,
                          size_t n, std::string& out) {
    while (n > 0) {
        if (!leftover.empty()) {
            size_t take = std::min(n, leftover.size());
            out.append(leftover, 0, take);
            leftover.erase(0, take);
            n -= take;
            continue;
        }
        char buf[4096];
        ssize_t got = conn.read_some(buf, std::min(n, sizeof(buf)));
        if (got <= 0) return false;
        out.append(buf, static_cast<size_t>(got));
        n -= static_cast<size_t>(got);
    }
    return true;
}

static void read_until_eof(Connection& conn, std::string& leftover,
                            std::string& out) {
    out += leftover;
    leftover.clear();
    char buf[4096];
    for (;;) {
        ssize_t n = conn.read_some(buf, sizeof(buf));
        if (n <= 0) break;
        out.append(buf, static_cast<size_t>(n));
    }
}

// Accumulate full body (handles chunked + content-length + read-to-close).
static std::string read_body(Connection& conn, std::string& leftover,
                              bool is_chunked, size_t content_length) {
    std::string body;
    if (is_chunked) {
        for (;;) {
            std::string size_line = read_line(conn, leftover);
            if (size_line.empty()) break;
            // Chunk size is hex, may have extensions after ';'
            size_t chunk_size = std::strtoul(size_line.c_str(), nullptr, 16);
            if (chunk_size == 0) break;
            if (!read_exactly(conn, leftover, chunk_size, body)) break;
            std::string crlf;
            read_exactly(conn, leftover, 2, crlf); // trailing \r\n
        }
    } else if (content_length > 0) {
        read_exactly(conn, leftover, content_length, body);
    } else {
        read_until_eof(conn, leftover, body);
    }
    return body;
}

// Stream body to a RawChunkCallback; dechunks if needed.
static bool stream_body_raw(Connection& conn, std::string& leftover,
                             bool is_chunked, size_t content_length,
                             RawChunkCallback& callback) {
    if (is_chunked) {
        for (;;) {
            std::string size_line = read_line(conn, leftover);
            if (size_line.empty()) break;
            size_t chunk_size = std::strtoul(size_line.c_str(), nullptr, 16);
            if (chunk_size == 0) break;

            size_t remaining = chunk_size;
            while (remaining > 0) {
                if (!leftover.empty()) {
                    size_t take = std::min(remaining, leftover.size());
                    if (!callback(leftover.c_str(), take)) return false;
                    leftover.erase(0, take);
                    remaining -= take;
                    continue;
                }
                char buf[4096];
                ssize_t n = conn.read_some(buf, std::min(remaining, sizeof(buf)));
                if (n <= 0) return true; // EOF mid-chunk is fine (server closed)
                if (!callback(buf, static_cast<size_t>(n))) return false;
                remaining -= static_cast<size_t>(n);
            }
            std::string crlf;
            read_exactly(conn, leftover, 2, crlf);
        }
    } else {
        bool use_length = (content_length > 0);
        size_t remaining = content_length;

        while (!use_length || remaining > 0) {
            if (!leftover.empty()) {
                size_t take = use_length
                    ? std::min(remaining, leftover.size())
                    : leftover.size();
                if (!callback(leftover.c_str(), take)) return false;
                leftover.erase(0, take);
                if (use_length) remaining -= take;
                continue;
            }
            size_t want = use_length
                ? std::min(remaining, static_cast<size_t>(4096))
                : 4096;
            char buf[4096];
            ssize_t n = conn.read_some(buf, want);
            if (n <= 0) break;
            if (!callback(buf, static_cast<size_t>(n))) return false;
            if (use_length) remaining -= static_cast<size_t>(n);
        }
    }
    return true;
}

// ── Core request executor ──────────────────────────────────────

static HttpResponse do_request(const std::string& method,
                                const std::string& url_str,
                                const std::string& body,
                                const std::vector<Header>& headers,
                                long timeout_secs) {
    ParsedUrl url;
    try { url = parse_url(url_str); } catch (...) { return {}; }

    Connection conn;
    if (!conn.connect(url, timeout_secs)) return {};

    std::string request = build_request(method, url, body, headers);
    if (!conn.write_all(request.c_str(), request.size())) return {};

    std::string leftover;
    bool   is_chunked     = false;
    size_t content_length = 0;
    long status = parse_response_headers(conn, leftover, is_chunked, content_length);
    if (status == 0) return {};

    HttpResponse resp;
    resp.status_code = status;
    resp.body = read_body(conn, leftover, is_chunked, content_length);
    return resp;
}

// ── Public API ─────────────────────────────────────────────────

HttpResponse SocketHttpClient::post(const std::string& url,
                                     const std::string& body,
                                     const std::vector<Header>& headers,
                                     long timeout_seconds) {
    return http_post(url, body, headers, timeout_seconds);
}

HttpResponse http_post(const std::string& url,
                       const std::string& body,
                       const std::vector<Header>& headers,
                       long timeout_seconds) {
    return do_request("POST", url, body, headers, timeout_seconds);
}

HttpResponse http_get(const std::string& url,
                      const std::vector<Header>& headers,
                      long timeout_seconds) {
    return do_request("GET", url, "", headers, timeout_seconds);
}

HttpResponse http_stream_post(const std::string& url,
                               const std::string& body,
                               const std::vector<Header>& headers,
                               StreamCallback callback,
                               long timeout_seconds) {
    // Wrap the line-based SSE callback as a raw-chunk callback.
    std::string line_buf;
    RawChunkCallback raw_cb = [&](const char* data, size_t len) -> bool {
        line_buf.append(data, len);
        size_t pos;
        while ((pos = line_buf.find('\n')) != std::string::npos) {
            std::string line = line_buf.substr(0, pos);
            line_buf.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind("data: ", 0) == 0) {
                if (!callback(line.substr(6))) return false;
            }
        }
        return true;
    };
    return http_stream_post_raw(url, body, headers, std::move(raw_cb), timeout_seconds);
}

// Default base-class implementation delegates to http_stream_post_raw.
HttpResponse HttpClient::stream_post_raw(const std::string& url,
                                          const std::string& body,
                                          const std::vector<Header>& headers,
                                          RawChunkCallback callback,
                                          long timeout_seconds) {
    return http_stream_post_raw(url, body, headers, std::move(callback), timeout_seconds);
}

HttpResponse http_stream_post_raw(const std::string& url,
                                   const std::string& body,
                                   const std::vector<Header>& headers,
                                   RawChunkCallback callback,
                                   long timeout_seconds) {
    ParsedUrl parsed_url;
    try { parsed_url = parse_url(url); } catch (...) { return {}; }

    Connection conn;
    if (!conn.connect(parsed_url, timeout_seconds)) return {};

    std::string request = build_request("POST", parsed_url, body, headers);
    if (!conn.write_all(request.c_str(), request.size())) return {};

    std::string leftover;
    bool   is_chunked     = false;
    size_t content_length = 0;
    long status = parse_response_headers(conn, leftover, is_chunked, content_length);
    if (status == 0) return {};

    stream_body_raw(conn, leftover, is_chunked, content_length, callback);
    return {status, ""};
}

} // namespace ptrclaw

#endif // __linux__
