#include "../../include/ptrclaw/ptrclaw.h"
#include "../channels/embed.hpp"
#include "../config.hpp"
#include "../event.hpp"
#include "../event_bus.hpp"
#include "../http.hpp"
#include "../plugin.hpp"
#include "../session.hpp"
#include "../stream_relay.hpp"
#include <nlohmann/json.hpp>
#include <atomic>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

// ── Host-bridged tool ───────────────────────────────────────────

namespace ptrclaw {

class BridgedTool : public Tool {
public:
    BridgedTool(std::string name, std::string desc, std::string params,
                PtrClawToolCallback cb, void* ud)
        : name_(std::move(name)), desc_(std::move(desc)),
          params_(std::move(params)), callback_(cb), userdata_(ud) {}

    std::string tool_name() const override { return name_; }
    std::string description() const override { return desc_; }
    std::string parameters_json() const override { return params_; }

    ToolResult execute(const std::string& args_json) override {
        char* result = callback_(name_.c_str(), args_json.c_str(), userdata_);
        if (!result) {
            return ToolResult{false, "tool callback returned null"};
        }
        std::string output(result);
        free(result);  // NOLINT(cppcoreguidelines-no-malloc)
        return ToolResult{true, std::move(output)};
    }

private:
    std::string name_;
    std::string desc_;
    std::string params_;
    PtrClawToolCallback callback_;
    void* userdata_;
};

} // namespace ptrclaw

// ── Opaque handle ────────────────────────────────────────────────

struct PtrClawHandle_ {
    ptrclaw::Config config;
    std::unique_ptr<ptrclaw::PlatformHttpClient> http;
    std::unique_ptr<ptrclaw::EventBus> bus;
    std::unique_ptr<ptrclaw::SessionManager> sessions;
    ptrclaw::EmbedChannel* channel = nullptr;  // non-owning; owned by unique_ptr below
    std::unique_ptr<ptrclaw::Channel> channel_owner;
    std::unique_ptr<ptrclaw::StreamRelay> relay;
    std::thread loop_thread;
    std::atomic<bool> shutdown{false};

    // Per-session last response cache
    std::mutex response_mutex;
    std::unordered_map<std::string, std::string> last_responses;

    std::string last_error;
};

// ── Background event loop (mirrors run_channel in main.cpp) ─────

static void embed_loop(PtrClawHandle_* h) {
    uint32_t poll_count = 0;
    while (!h->shutdown.load()) {
        auto messages = h->channel->poll_updates();
        for (auto& msg : messages) {
            ptrclaw::MessageReceivedEvent ev;
            ev.session_id = msg.sender;
            ev.message = std::move(msg);
            h->bus->publish(ev);
        }
        if (++poll_count % 100 == 0) {
            h->sessions->evict_idle(3600);
        }
    }
}

// ── Version ──────────────────────────────────────────────────────

extern "C" const char* ptrclaw_version(void) {
    return PTRCLAW_VERSION_STRING;
}

// ── Host-bridged tool registration ───────────────────────────────

extern "C" int ptrclaw_register_tool(const char* name,
                                      const char* description,
                                      const char* parameters_json,
                                      PtrClawToolCallback callback,
                                      void* userdata) {
    if (!name || !description || !parameters_json || !callback)
        return PTRCLAW_ERR_INVALID;

    std::string n = name;
    std::string d = description;
    std::string p = parameters_json;
    ptrclaw::PluginRegistry::instance().register_tool(n,
        [n, d, p, callback, userdata]() -> std::unique_ptr<ptrclaw::Tool> {
            return std::make_unique<ptrclaw::BridgedTool>(n, d, p, callback, userdata);
        });

    return PTRCLAW_OK;
}

// ── Lifecycle ────────────────────────────────────────────────────

extern "C" PtrClawHandle ptrclaw_create(const char* config_json) {
    auto* h = new PtrClawHandle_();

    try {
        ptrclaw::http_init();
        h->config = ptrclaw::Config::load();

        // Apply JSON overrides
        if (config_json) {
            try {
                auto j = nlohmann::json::parse(config_json);
                if (j.contains("provider") && j["provider"].is_string())
                    h->config.provider = j["provider"].get<std::string>();
                if (j.contains("model") && j["model"].is_string())
                    h->config.model = j["model"].get<std::string>();
                if (j.contains("temperature") && j["temperature"].is_number())
                    h->config.temperature = j["temperature"].get<double>();
            } catch (const std::exception& e) {
                h->last_error = std::string("config parse error: ") + e.what();
                // Continue with base config
            }
        }

        h->http = std::make_unique<ptrclaw::PlatformHttpClient>();

        // Create the embed channel
        h->channel_owner = std::make_unique<ptrclaw::EmbedChannel>();
        h->channel = static_cast<ptrclaw::EmbedChannel*>(h->channel_owner.get());

        // Wire up EventBus + SessionManager + StreamRelay (same as run_channel)
        h->bus = std::make_unique<ptrclaw::EventBus>();
        h->sessions = std::make_unique<ptrclaw::SessionManager>(h->config, *h->http);
        h->sessions->set_event_bus(h->bus.get());

        h->relay = std::make_unique<ptrclaw::StreamRelay>(*h->channel, *h->bus);
        h->relay->subscribe_events();
        h->sessions->subscribe_events();

        // Subscribe to StreamChunkEvent to forward to host callbacks
        ptrclaw::subscribe<ptrclaw::StreamChunkEvent>(*h->bus,
            [h](const ptrclaw::StreamChunkEvent& ev) {
                auto cb = h->channel->get_stream_callback(ev.session_id);
                if (cb && !ev.delta.empty()) {
                    cb(ev.delta.c_str(), 0);
                }
            });

        // Subscribe to MessageReadyEvent to cache last responses
        ptrclaw::subscribe<ptrclaw::MessageReadyEvent>(*h->bus,
            [h](const ptrclaw::MessageReadyEvent& ev) {
                std::lock_guard<std::mutex> lock(h->response_mutex);
                h->last_responses[ev.session_id] = ev.content;
            });

        // Start background loop
        h->loop_thread = std::thread(embed_loop, h);

    } catch (const std::exception& e) {
        h->last_error = std::string("create failed: ") + e.what();
        delete h;
        return nullptr;
    }

    return h;
}

extern "C" void ptrclaw_destroy(PtrClawHandle handle) {
    if (!handle) return;
    handle->shutdown.store(true);
    if (handle->loop_thread.joinable()) {
        handle->loop_thread.join();
    }
    delete handle;
    ptrclaw::http_cleanup();
}

extern "C" const char* ptrclaw_last_error(PtrClawHandle handle) {
    if (!handle) return "null handle";
    return handle->last_error.c_str();
}

// ── Messaging ────────────────────────────────────────────────────

extern "C" int ptrclaw_send(PtrClawHandle handle, const char* session_id,
                             const char* message) {
    if (!handle || !session_id || !message) return PTRCLAW_ERR_INVALID;
    if (!handle->channel) return PTRCLAW_ERR_INVALID;

    try {
        std::string response = handle->channel->send_user_message(
            session_id, message);
        std::lock_guard<std::mutex> lock(handle->response_mutex);
        handle->last_responses[session_id] = std::move(response);
        return PTRCLAW_OK;
    } catch (const std::exception& e) {
        handle->last_error = std::string("send failed: ") + e.what();
        return PTRCLAW_ERR_PROVIDER;
    }
}

extern "C" const char* ptrclaw_last_response(PtrClawHandle handle,
                                              const char* session_id) {
    static const char* empty = "";
    if (!handle || !session_id) return empty;
    std::lock_guard<std::mutex> lock(handle->response_mutex);
    auto it = handle->last_responses.find(session_id);
    if (it != handle->last_responses.end()) return it->second.c_str();
    return empty;
}

extern "C" int ptrclaw_send_stream(PtrClawHandle handle,
                                    const char* session_id,
                                    const char* message,
                                    PtrClawChunkCallback on_chunk,
                                    void* userdata) {
    if (!handle || !session_id || !message || !on_chunk)
        return PTRCLAW_ERR_INVALID;
    if (!handle->channel) return PTRCLAW_ERR_INVALID;

    try {
        auto callback = [on_chunk, userdata](const char* chunk, int done) {
            on_chunk(chunk, done, userdata);
        };

        std::string response = handle->channel->send_user_message_stream(
            session_id, message, std::move(callback));

        // Send terminal sentinel
        on_chunk("", 1, userdata);

        std::lock_guard<std::mutex> lock(handle->response_mutex);
        handle->last_responses[session_id] = std::move(response);
        return PTRCLAW_OK;
    } catch (const std::exception& e) {
        handle->last_error = std::string("stream failed: ") + e.what();
        return PTRCLAW_ERR_PROVIDER;
    }
}
