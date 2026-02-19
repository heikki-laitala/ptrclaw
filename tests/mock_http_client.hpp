#pragma once
#include "http.hpp"

namespace ptrclaw {

class MockHttpClient : public HttpClient {
public:
    HttpResponse next_response;
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
        return next_response;
    }
};

} // namespace ptrclaw
