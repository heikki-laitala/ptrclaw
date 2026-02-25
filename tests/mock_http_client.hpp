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
    int call_count = 0;

    HttpResponse post(const std::string& url,
                      const std::string& body,
                      const std::vector<Header>& headers,
                      long /*timeout_seconds*/) override {
        call_count++;
        last_url = url;
        last_body = body;
        last_headers = headers;
        if (!response_queue.empty()) {
            auto resp = response_queue.front();
            response_queue.erase(response_queue.begin());
            return resp;
        }
        return next_response;
    }
};

} // namespace ptrclaw
