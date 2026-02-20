#pragma once
#include <string>
#include <vector>
#include <functional>
#include <utility>

namespace ptrclaw {

// Initialize libcurl (call once at startup)
void http_init();

// Cleanup libcurl (call once at shutdown)
void http_cleanup();

using Header = std::pair<std::string, std::string>;

struct HttpResponse {
    long status_code = 0;
    std::string body;
};

// Raw-chunk streaming callback: receives raw bytes from the response.
// Return false to abort the stream.
using RawChunkCallback = std::function<bool(const char* data, size_t len)>;

// Abstract HTTP client interface (injectable for testing)
class HttpClient {
public:
    virtual ~HttpClient() = default;
    virtual HttpResponse post(const std::string& url,
                              const std::string& body,
                              const std::vector<Header>& headers,
                              long timeout_seconds = 120) = 0;

    virtual HttpResponse stream_post_raw(const std::string& url,
                                         const std::string& body,
                                         const std::vector<Header>& headers,
                                         RawChunkCallback callback,
                                         long timeout_seconds = 300);
};

// Concrete implementation using libcurl
class CurlHttpClient : public HttpClient {
public:
    HttpResponse post(const std::string& url,
                      const std::string& body,
                      const std::vector<Header>& headers,
                      long timeout_seconds = 120) override;
};

// HTTP POST with JSON body
HttpResponse http_post(const std::string& url,
                       const std::string& body,
                       const std::vector<Header>& headers,
                       long timeout_seconds = 120);

// HTTP GET
HttpResponse http_get(const std::string& url,
                      const std::vector<Header>& headers,
                      long timeout_seconds = 30);

// SSE streaming callback: receives each line of SSE data
// Return false to abort the stream.
using StreamCallback = std::function<bool(const std::string& data)>;

// HTTP POST with SSE streaming response
HttpResponse http_stream_post(const std::string& url,
                              const std::string& body,
                              const std::vector<Header>& headers,
                              StreamCallback callback,
                              long timeout_seconds = 300);

// HTTP POST with raw-chunk streaming (no SSE parsing — caller parses)
HttpResponse http_stream_post_raw(const std::string& url,
                                  const std::string& body,
                                  const std::vector<Header>& headers,
                                  RawChunkCallback callback,
                                  long timeout_seconds = 300);

} // namespace ptrclaw
