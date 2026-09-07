#include "context_search.hpp"
#include "tool_util.hpp"
#include "../plugin.hpp"
#include <nlohmann/json.hpp>

// Registered only in a serving build, like the scoped file tools: the class compiles
// everywhere so the default suite exercises it, and with no registrar referencing it LTO
// drops the code from a personal binary.
#ifdef PTRCLAW_HAS_SERVING
static ptrclaw::ToolRegistrar reg_context_search("context_search",
    []() { return std::make_unique<ptrclaw::ContextSearchTool>(); });
#endif

namespace ptrclaw {

namespace {

// The same ceiling the scoped file read uses, and for the same reason: one oversized answer
// must not allocate its way through a pod every other session is sharing. It is also the
// point past which a model stops being helped by more text.
constexpr size_t kMaxAnswerBytes = 50000;

// Percent-encode everything a query string may not carry literally.
//
// ⚠ THE QUERY IS MODEL-WRITTEN TEXT, so it reaches here with spaces, quotes and newlines in
// it. Pasted into a URL unescaped, a "&" would silently split it into a second parameter and
// the service would answer a different question than the model asked.
std::string url_encode(const std::string& in) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(in.size() + 16);
    for (unsigned char c : in) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

// Pull readable text out of whatever the service answered.
//
// ⚠ FORGIVING ON PURPOSE, BECAUSE THE SERVICE IS THE DEPLOYMENT'S CHOICE. Two shapes are
// understood — a top-level array, or an object carrying one under "results", "memories",
// "items" or "data" — and within an entry either "content" or "text". Anything else is
// handed to the model verbatim rather than dropped: a service that answered something is
// more use to it than a tool that says nothing, and silently returning empty is how a
// retrieval path looks healthy while delivering nothing.
std::string readable(const std::string& body) {
    nlohmann::json doc = nlohmann::json::parse(body, nullptr, false);
    if (doc.is_discarded()) return body;

    const nlohmann::json* arr = nullptr;
    if (doc.is_array()) {
        arr = &doc;
    } else if (doc.is_object()) {
        for (const char* key : {"results", "memories", "items", "data"}) {
            if (doc.contains(key) && doc[key].is_array()) { arr = &doc[key]; break; }
        }
    }
    if (arr == nullptr) return body;

    std::string out;
    for (const auto& entry : *arr) {
        std::string text;
        if (entry.is_string()) {
            text = entry.get<std::string>();
        } else if (entry.is_object()) {
            for (const char* key : {"content", "text"}) {
                if (entry.contains(key) && entry[key].is_string()) {
                    text = entry[key].get<std::string>();
                    break;
                }
            }
        }
        if (text.empty()) continue;
        if (!out.empty()) out += "\n\n";
        out += text;
    }
    // An array that parsed but carried nothing readable is still an answer about the world:
    // say so rather than returning the raw JSON, which would read to the model as content.
    if (out.empty()) return "";
    return out;
}

} // namespace

std::string ContextSearchTool::description() const {
    return "Search the shared knowledge for this business and return the passages that match. "
           "Use it when a question needs a detail you have not been given — a price, a policy, "
           "an opening time — rather than answering from what is usual.";
}

std::string ContextSearchTool::parameters_json() const {
    return R"({"type":"object","properties":{)"
           R"("query":{"type":"string","description":"What to look for, in plain language."},)"
           R"("limit":{"type":"integer","description":"How many passages to return. Default 5."})"
           R"(},"required":["query"]})";
}

ToolResult ContextSearchTool::execute(const std::string& args_json) {
    if (recall_url_.empty()) {
        // Reached only if a build registers the tool without configuring it. Say which,
        // because "no results" would read to the model as "the business has no such note".
        return ToolResult{false, "No knowledge service is configured for this agent."};
    }
    nlohmann::json args = nlohmann::json::parse(args_json, nullptr, false);
    if (args.is_discarded() || !args.contains("query") || !args["query"].is_string()) {
        return ToolResult{false, "context_search needs a \"query\" string."};
    }
    const std::string query = args["query"].get<std::string>();
    if (query.empty()) {
        return ToolResult{false, "context_search needs a non-empty \"query\"."};
    }

    std::string url = recall_url_ + (recall_url_.find('?') == std::string::npos ? "?" : "&") +
                      "q=" + url_encode(query);
    if (args.contains("limit") && args["limit"].is_number_integer()) {
        long long limit = args["limit"].get<long long>();
        if (limit > 0 && limit <= 50) url += "&limit=" + std::to_string(limit);
    }

    PlatformHttpClient platform;
    HttpClient* http = client_ != nullptr ? client_ : &platform;
    HttpResponse res = http->get(url, {});

    if (res.status_code < 200 || res.status_code >= 300) {
        // ⚠ THE STATUS, NEVER THE BODY. An error body from a service holding other tenants'
        // material is not something to hand a model that is talking to a stranger.
        return ToolResult{false,
            "The knowledge service answered " + std::to_string(res.status_code) + "."};
    }

    std::string text = readable(res.body);
    if (text.empty()) {
        // ⚠ NOT AN ERROR. "Nothing matched" is a true answer and the model should say so,
        // rather than treating it as a fault and trying again.
        return ToolResult{true, "No matching notes."};
    }
    if (text.size() > kMaxAnswerBytes) {
        text.resize(kMaxAnswerBytes);
        text += "\n\n[truncated]";
    }
    return ToolResult{true, text};
}

} // namespace ptrclaw
