#pragma once
#include "http.hpp"

namespace ptrclaw {

class MockHttpClient : public HttpClient {
public:
    HttpResponse next_response;
    std::vector<HttpResponse> response_queue;
    std::string last_url;
    std::string last_body;
    std::vector<Header> last_headers;
    long last_timeout = 0;
    std::vector<long> timeouts;
    int call_count = 0;

    HttpResponse post(const std::string& url,
                      const std::string& body,
                      const std::vector<Header>& headers,
                      long timeout_seconds) override {
        call_count++;
        last_url = url;
        last_body = body;
        last_headers = headers;
        last_timeout = timeout_seconds;
        timeouts.push_back(timeout_seconds);
        if (!response_queue.empty()) {
            auto resp = response_queue.front();
            response_queue.erase(response_queue.begin());
            return resp;
        }
        return next_response;
    }

    // Raw SSE bytes to feed a streaming call, delivered in one chunk. Providers parse the
    // stream themselves, so one chunk exercises the same parser as many — what it does not
    // exercise is a frame split across chunk boundaries, which is the transport's problem
    // rather than the provider's.
    std::string next_stream_body;

    HttpResponse stream_post_raw(const std::string& url,
                                 const std::string& body,
                                 const std::vector<Header>& headers,
                                 RawChunkCallback callback,
                                 long timeout_seconds) override {
        call_count++;
        last_url = url;
        last_body = body;
        last_headers = headers;
        last_timeout = timeout_seconds;
        timeouts.push_back(timeout_seconds);
        if (!next_stream_body.empty() && callback) {
            callback(next_stream_body.data(), next_stream_body.size());
        }
        HttpResponse resp = next_response;
        if (resp.status_code == 0) resp.status_code = 200;
        return resp;
    }
};

} // namespace ptrclaw
