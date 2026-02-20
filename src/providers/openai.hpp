#pragma once
#include "../provider.hpp"
#include "../http.hpp"
#include <nlohmann/json.hpp>
#include <string>

namespace ptrclaw {

class OpenAIProvider : public Provider {
public:
    OpenAIProvider(const std::string& api_key, HttpClient& http,
                   const std::string& base_url = "https://api.openai.com/v1");

    ChatResponse chat(const std::vector<ChatMessage>& messages,
                      const std::vector<ToolSpec>& tools,
                      const std::string& model,
                      double temperature) override;

    ChatResponse chat_stream(const std::vector<ChatMessage>& messages,
                             const std::vector<ToolSpec>& tools,
                             const std::string& model,
                             double temperature,
                             const TextDeltaCallback& on_delta) override;

    std::string chat_simple(const std::string& system_prompt,
                            const std::string& message,
                            const std::string& model,
                            double temperature) override;

    bool supports_native_tools() const override { return true; }
    bool supports_streaming() const override { return true; }
    std::string provider_name() const override { return "openai"; }

protected:
    nlohmann::json build_request(const std::vector<ChatMessage>& messages,
                                 const std::vector<ToolSpec>& tools,
                                 const std::string& model,
                                 double temperature) const;
    virtual std::vector<Header> build_headers() const;

    std::string api_key_;
    HttpClient& http_;
    std::string base_url_;
};

} // namespace ptrclaw
