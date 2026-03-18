#pragma once
#include <string>
#include <vector>
#include <utility>
#include <cstdint>

namespace ptrclaw {

// ── Pending OAuth state (provider-agnostic) ──────────────────────
struct PendingOAuth {
    std::string provider;
    std::string state;
    std::string code_verifier;
    std::string redirect_uri;
    uint64_t created_at = 0;
};

constexpr uint64_t kPendingOAuthExpirySeconds = 600; // 10 minutes

// ── OAuth flow start result ──────────────────────────────────────
struct OAuthFlowStart {
    PendingOAuth pending;
    std::string authorize_url;
};

// ── Parsed OAuth callback input ──────────────────────────────────
struct ParsedOAuthInput {
    std::string code;
    std::string state;
};

// ── Generic OAuth utilities ──────────────────────────────────────
std::string oauth_url_encode(const std::string& s);
std::string form_encode(const std::vector<std::pair<std::string, std::string>>& params);
std::string make_code_verifier();
std::string make_code_challenge_s256(const std::string& verifier);
ParsedOAuthInput parse_oauth_input(const std::string& raw_input);

} // namespace ptrclaw
