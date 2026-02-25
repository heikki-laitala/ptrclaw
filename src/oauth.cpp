#include "oauth.hpp"
#include "session.hpp"
#include "util.hpp"

#ifdef PTRCLAW_USE_COMMONCRYPTO
#include <CommonCrypto/CommonDigest.h>
#else
#include <openssl/sha.h>
#endif
#include <nlohmann/json.hpp>
#include <sstream>
#include <iomanip>
#include <fstream>

namespace ptrclaw {

using json = nlohmann::json;

namespace {

std::string base64url_encode(const unsigned char* data, size_t len) {
    static const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve((len * 4 + 2) / 3);

    size_t i = 0;
    while (i + 3 <= len) {
        uint32_t n = (static_cast<uint32_t>(data[i]) << 16) |
                     (static_cast<uint32_t>(data[i + 1]) << 8) |
                     static_cast<uint32_t>(data[i + 2]);
        out.push_back(alphabet[(n >> 18) & 63]);
        out.push_back(alphabet[(n >> 12) & 63]);
        out.push_back(alphabet[(n >> 6) & 63]);
        out.push_back(alphabet[n & 63]);
        i += 3;
    }

    if (i < len) {
        uint32_t n = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < len) n |= static_cast<uint32_t>(data[i + 1]) << 8;
        out.push_back(alphabet[(n >> 18) & 63]);
        out.push_back(alphabet[(n >> 12) & 63]);
        if (i + 1 < len) {
            out.push_back(alphabet[(n >> 6) & 63]);
        }
    }

    return out;
}

std::string query_param(const std::string& input, const std::string& key) {
    auto qpos = input.find('?');
    std::string query = (qpos == std::string::npos) ? input : input.substr(qpos + 1);
    auto hash = query.find('#');
    if (hash != std::string::npos) query = query.substr(0, hash);

    auto parts = split(query, '&');
    for (const auto& p : parts) {
        auto eq = p.find('=');
        if (eq == std::string::npos) continue;
        if (p.substr(0, eq) == key) {
            return p.substr(eq + 1);
        }
    }
    return "";
}

} // namespace

// ── URL encoding ─────────────────────────────────────────────────

std::string oauth_url_encode(const std::string& s) {
    std::ostringstream out;
    out << std::hex << std::uppercase;
    for (unsigned char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out << c;
        } else {
            out << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(c);
        }
    }
    return out.str();
}

// ── Form encoding ────────────────────────────────────────────────

std::string form_encode(const std::vector<std::pair<std::string, std::string>>& params) {
    std::string result;
    for (const auto& [key, value] : params) {
        if (!result.empty()) result += '&';
        result += oauth_url_encode(key) + '=' + oauth_url_encode(value);
    }
    return result;
}

// ── PKCE helpers ─────────────────────────────────────────────────

std::string make_code_verifier() {
    auto id = generate_id() + generate_id();
    return base64url_encode(reinterpret_cast<const unsigned char*>(id.data()), id.size());
}

std::string make_code_challenge_s256(const std::string& verifier) {
#ifdef PTRCLAW_USE_COMMONCRYPTO
    unsigned char hash[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(verifier.data(), static_cast<CC_LONG>(verifier.size()), hash);
    return base64url_encode(hash, CC_SHA256_DIGEST_LENGTH);
#else
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(verifier.data()), verifier.size(), hash);
    return base64url_encode(hash, SHA256_DIGEST_LENGTH);
#endif
}

// ── Authorize URL builder ────────────────────────────────────────

std::string build_authorize_url(const std::string& client_id,
                                const std::string& redirect_uri,
                                const std::string& code_challenge,
                                const std::string& state) {
    std::string scope = "openid profile email offline_access";
    return std::string(kDefaultAuthorizeBaseUrl) +
        "?response_type=code"
        "&client_id=" + oauth_url_encode(client_id) +
        "&redirect_uri=" + oauth_url_encode(redirect_uri) +
        "&scope=" + oauth_url_encode(scope) +
        "&state=" + oauth_url_encode(state) +
        "&code_challenge=" + oauth_url_encode(code_challenge) +
        "&code_challenge_method=S256"
        "&id_token_add_organizations=true"
        "&codex_cli_simplified_flow=true";
}

// ── OAuth input parsing ──────────────────────────────────────────

ParsedOAuthInput parse_oauth_input(const std::string& raw_input) {
    std::string input = trim(raw_input);
    ParsedOAuthInput result;
    result.code = input;
    result.state = query_param(input, "state");
    std::string code_from_query = query_param(input, "code");
    if (!code_from_query.empty()) result.code = code_from_query;
    return result;
}

// ── Token exchange ───────────────────────────────────────────────

std::string exchange_oauth_token(const std::string& code,
                                  const PendingOAuth& pending,
                                  const ProviderEntry& openai_entry,
                                  HttpClient& http,
                                  ProviderEntry& out_entry) {
    std::string token_url = openai_entry.oauth_token_url.empty()
        ? kDefaultTokenUrl
        : openai_entry.oauth_token_url;
    std::string client_id = openai_entry.oauth_client_id.empty()
        ? kDefaultOAuthClientId
        : openai_entry.oauth_client_id;

    try {
        std::string body = form_encode({
            {"grant_type", "authorization_code"},
            {"code", code},
            {"redirect_uri", pending.redirect_uri},
            {"code_verifier", pending.code_verifier},
            {"client_id", client_id}
        });
        auto resp = http.post(token_url, body,
                              {{"Content-Type", "application/x-www-form-urlencoded"}}, 120);

        if (resp.status_code < 200 || resp.status_code >= 300) {
            return "Token exchange failed (HTTP " +
                   std::to_string(resp.status_code) + ").";
        }

        auto tok = json::parse(resp.body);
        std::string access = tok.value("access_token", "");
        if (access.empty()) {
            return "Token exchange succeeded but access_token is missing.";
        }
        std::string refresh = tok.value("refresh_token", "");
        uint64_t expires_in = tok.value("expires_in", 3600u);

        out_entry = openai_entry;
        out_entry.use_oauth = true;
        out_entry.oauth_access_token = access;
        if (!refresh.empty()) out_entry.oauth_refresh_token = refresh;
        out_entry.oauth_expires_at = epoch_seconds() + expires_in;
        if (out_entry.oauth_client_id.empty()) out_entry.oauth_client_id = client_id;
        if (out_entry.oauth_token_url.empty()) out_entry.oauth_token_url = token_url;
    } catch (const std::exception& e) {
        return std::string("OpenAI auth failed: ") + e.what();
    }

    return "";
}

// ── Config persistence ───────────────────────────────────────────

bool persist_openai_oauth(const ProviderEntry& entry) {
    std::string path = expand_home("~/.ptrclaw/config.json");
    json j;
    {
        std::ifstream in(path);
        if (!in.is_open()) return false;
        try { in >> j; } catch (...) { return false; }
    }

    if (!j.contains("providers") || !j["providers"].is_object()) {
        j["providers"] = json::object();
    }
    if (!j["providers"].contains("openai") || !j["providers"]["openai"].is_object()) {
        j["providers"]["openai"] = json::object();
    }

    auto& o = j["providers"]["openai"];
    o["use_oauth"] = entry.use_oauth;
    o["oauth_access_token"] = entry.oauth_access_token;
    o["oauth_refresh_token"] = entry.oauth_refresh_token;
    o["oauth_expires_at"] = entry.oauth_expires_at;
    o["oauth_client_id"] = entry.oauth_client_id;
    o["oauth_token_url"] = entry.oauth_token_url;

    return atomic_write_file(path, j.dump(4) + "\n");
}

} // namespace ptrclaw
