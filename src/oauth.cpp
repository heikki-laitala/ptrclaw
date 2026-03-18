#include "oauth.hpp"
#include "util.hpp"

#ifdef PTRCLAW_USE_COMMONCRYPTO
#include <CommonCrypto/CommonDigest.h>
#elif defined(PTRCLAW_USE_MBEDTLS)
#include <mbedtls/version.h>
#if MBEDTLS_VERSION_MAJOR >= 4
#include <mbedtls/md.h>
#else
#include <mbedtls/sha256.h>
#endif
#else
#include <openssl/sha.h>
#endif
#include <random>
#include <sstream>
#include <iomanip>

namespace ptrclaw {

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
    // 32 random bytes → base64url (43 chars), matching PKCE spec
    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_int_distribution<unsigned> dist(0, 255);
    unsigned char buf[32];
    for (auto& b : buf) b = static_cast<unsigned char>(dist(gen));
    return base64url_encode(buf, sizeof(buf));
}

std::string make_code_challenge_s256(const std::string& verifier) {
#ifdef PTRCLAW_USE_COMMONCRYPTO
    unsigned char hash[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(verifier.data(), static_cast<CC_LONG>(verifier.size()), hash);
    return base64url_encode(hash, CC_SHA256_DIGEST_LENGTH);
#elif defined(PTRCLAW_USE_MBEDTLS)
    unsigned char hash[32];
#if MBEDTLS_VERSION_MAJOR >= 4
    mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
               reinterpret_cast<const unsigned char*>(verifier.data()),
               verifier.size(), hash);
#elif MBEDTLS_VERSION_MAJOR >= 3
    mbedtls_sha256(reinterpret_cast<const unsigned char*>(verifier.data()),
                   verifier.size(), hash, 0);
#else
    mbedtls_sha256_ret(reinterpret_cast<const unsigned char*>(verifier.data()),
                       verifier.size(), hash, 0);
#endif
    return base64url_encode(hash, sizeof(hash));
#else
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(verifier.data()), verifier.size(), hash);
    return base64url_encode(hash, SHA256_DIGEST_LENGTH);
#endif
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

} // namespace ptrclaw
