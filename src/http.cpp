#include "http.hpp"

#include <curl/curl.h>
#include <string>

namespace ptrclaw {

void http_init() {
    curl_global_init(CURL_GLOBAL_ALL);
}

void http_cleanup() {
    curl_global_cleanup();
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

static curl_slist* build_headers(const std::vector<Header>& headers) {
    curl_slist* list = nullptr;
    for (const auto& h : headers) {
        std::string entry = h.first + ": " + h.second;
        list = curl_slist_append(list, entry.c_str());
    }
    return list;
}

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
    HttpResponse response;
    CURL* curl = curl_easy_init();
    if (!curl) return response;

    curl_slist* hlist = build_headers(headers);

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hlist);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds);

    CURLcode res = curl_easy_perform(curl);
    if (res == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status_code);
    }

    curl_slist_free_all(hlist);
    curl_easy_cleanup(curl);
    return response;
}

HttpResponse http_get(const std::string& url,
                      const std::vector<Header>& headers,
                      long timeout_seconds) {
    HttpResponse response;
    CURL* curl = curl_easy_init();
    if (!curl) return response;

    curl_slist* hlist = build_headers(headers);

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hlist);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds);

    CURLcode res = curl_easy_perform(curl);
    if (res == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status_code);
    }

    curl_slist_free_all(hlist);
    curl_easy_cleanup(curl);
    return response;
}

HttpResponse http_stream_post(const std::string& url,
                              const std::string& body,
                              const std::vector<Header>& headers,
                              StreamCallback callback,
                              long timeout_seconds) {
    HttpResponse response;
    CURL* curl = curl_easy_init();
    if (!curl) return response;

    StreamContext ctx;
    ctx.callback = &callback;

    curl_slist* hlist = build_headers(headers);

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hlist);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stream_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds);

    CURLcode res = curl_easy_perform(curl);
    if (res == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status_code);
    }

    curl_slist_free_all(hlist);
    curl_easy_cleanup(curl);
    return response;
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
    HttpResponse response;
    CURL* curl = curl_easy_init();
    if (!curl) return response;

    RawStreamContext ctx;
    ctx.callback = &callback;

    curl_slist* hlist = build_headers(headers);

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hlist);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, raw_stream_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds);

    CURLcode res = curl_easy_perform(curl);
    if (res == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status_code);
    }

    curl_slist_free_all(hlist);
    curl_easy_cleanup(curl);
    return response;
}

} // namespace ptrclaw
