#include "http.hpp"

#include <curl/curl.h>
#include <string>

namespace ptrclaw {

static const std::atomic<bool>* g_http_abort_flag = nullptr;

void http_init() {
    curl_global_init(CURL_GLOBAL_ALL);
}

void http_cleanup() {
    curl_global_cleanup();
}

void http_set_abort_flag(const std::atomic<bool>* flag) {
    g_http_abort_flag = flag;
}

// Called by curl ~once per second; return non-zero to abort the transfer.
static int abort_progress_cb(void* /*clientp*/,
                              curl_off_t /*dltotal*/, curl_off_t /*dlnow*/,
                              curl_off_t /*ultotal*/, curl_off_t /*ulnow*/) {
    if (g_http_abort_flag && g_http_abort_flag->load(std::memory_order_relaxed))
        return 1;
    return 0;
}

static void apply_abort_hook(CURL* curl) {
    if (g_http_abort_flag) {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, abort_progress_cb);
    }
}

static size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    size_t total = size * nmemb;
    auto* body = static_cast<std::string*>(userdata);
    body->append(ptr, total);
    return total;
}

struct StreamContext {
    StreamCallback* callback;
    std::string buffer;
    bool aborted = false;
};

static size_t stream_write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    size_t total = size * nmemb;
    auto* ctx = static_cast<StreamContext*>(userdata);
    if (ctx->aborted) return 0;

    ctx->buffer.append(ptr, total);

    // Process complete lines
    size_t pos;
    while ((pos = ctx->buffer.find('\n')) != std::string::npos) {
        std::string line = ctx->buffer.substr(0, pos);
        ctx->buffer.erase(0, pos + 1);

        // Strip trailing \r
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        // Pass SSE data lines to the callback
        if (line.rfind("data: ", 0) == 0) {
            std::string data = line.substr(6);
            if (!(*ctx->callback)(data)) {
                ctx->aborted = true;
                return 0;
            }
        }
    }

    return total;
}

struct RawStreamContext {
    RawChunkCallback* callback;
    bool aborted = false;
};

static size_t raw_stream_write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    size_t total = size * nmemb;
    auto* ctx = static_cast<RawStreamContext*>(userdata);
    if (ctx->aborted) return 0;

    if (!(*ctx->callback)(ptr, total)) {
        ctx->aborted = true;
        return 0;
    }

    return total;
}

static curl_slist* build_headers(const std::vector<Header>& headers) {
    curl_slist* list = nullptr;
    for (const auto& h : headers) {
        std::string entry = h.first + ": " + h.second;
        list = curl_slist_append(list, entry.c_str());
    }
    return list;
}

// ── RAII curl handle with common setup ────────────────────────

struct CurlRequest {
    CURL* curl = curl_easy_init();
    curl_slist* hlist = nullptr;

    CurlRequest() = default;
    ~CurlRequest() {
        curl_slist_free_all(hlist);
        if (curl) curl_easy_cleanup(curl);
    }
    CurlRequest(const CurlRequest&) = delete;
    CurlRequest& operator=(const CurlRequest&) = delete;

    explicit operator bool() const { return curl != nullptr; }
};

static void setup_request(CurlRequest& req, const std::string& url,
                           const std::vector<Header>& headers, long timeout) {
    req.hlist = build_headers(headers);
    curl_easy_setopt(req.curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(req.curl, CURLOPT_HTTPHEADER, req.hlist);
    curl_easy_setopt(req.curl, CURLOPT_TIMEOUT, timeout);
    apply_abort_hook(req.curl);
}

static void set_post_body(CURL* curl, const std::string& body) {
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
}

static HttpResponse perform(CURL* curl) {
    HttpResponse response;
    CURLcode res = curl_easy_perform(curl);
    if (res == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status_code);
    return response;
}

// ── Public API ────────────────────────────────────────────────

HttpResponse CurlHttpClient::post(const std::string& url,
                                   const std::string& body,
                                   const std::vector<Header>& headers,
                                   long timeout_seconds) {
    return http_post(url, body, headers, timeout_seconds);
}

HttpResponse http_post(const std::string& url,
                       const std::string& body,
                       const std::vector<Header>& headers,
                       long timeout_seconds) {
    CurlRequest req;
    if (!req) return {};
    setup_request(req, url, headers, timeout_seconds);
    set_post_body(req.curl, body);
    curl_easy_setopt(req.curl, CURLOPT_WRITEFUNCTION, write_callback);
    HttpResponse response;
    curl_easy_setopt(req.curl, CURLOPT_WRITEDATA, &response.body);
    CURLcode res = curl_easy_perform(req.curl);
    if (res == CURLE_OK)
        curl_easy_getinfo(req.curl, CURLINFO_RESPONSE_CODE, &response.status_code);
    return response;
}

HttpResponse http_get(const std::string& url,
                      const std::vector<Header>& headers,
                      long timeout_seconds) {
    CurlRequest req;
    if (!req) return {};
    setup_request(req, url, headers, timeout_seconds);
    curl_easy_setopt(req.curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(req.curl, CURLOPT_WRITEFUNCTION, write_callback);
    HttpResponse response;
    curl_easy_setopt(req.curl, CURLOPT_WRITEDATA, &response.body);
    CURLcode res = curl_easy_perform(req.curl);
    if (res == CURLE_OK)
        curl_easy_getinfo(req.curl, CURLINFO_RESPONSE_CODE, &response.status_code);
    return response;
}

HttpResponse http_stream_post(const std::string& url,
                              const std::string& body,
                              const std::vector<Header>& headers,
                              StreamCallback callback,
                              long timeout_seconds) {
    CurlRequest req;
    if (!req) return {};
    setup_request(req, url, headers, timeout_seconds);
    set_post_body(req.curl, body);
    StreamContext ctx;
    ctx.callback = &callback;
    curl_easy_setopt(req.curl, CURLOPT_WRITEFUNCTION, stream_write_callback);
    curl_easy_setopt(req.curl, CURLOPT_WRITEDATA, &ctx);
    return perform(req.curl);
}

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
    CurlRequest req;
    if (!req) return {};
    setup_request(req, url, headers, timeout_seconds);
    set_post_body(req.curl, body);
    RawStreamContext ctx;
    ctx.callback = &callback;
    curl_easy_setopt(req.curl, CURLOPT_WRITEFUNCTION, raw_stream_write_callback);
    curl_easy_setopt(req.curl, CURLOPT_WRITEDATA, &ctx);
    return perform(req.curl);
}

} // namespace ptrclaw
