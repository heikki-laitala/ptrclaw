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
#include <functional>
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
//
// Destruction order matters: the background thread must stop before any
// member it touches is destroyed.  Members are destroyed in reverse
// declaration order, so we declare them from longest-lived to shortest:
//
//   config, http  — no back-references, destroyed last
//   channel       — touched by the loop thread and relay
//   bus           — touched by the loop thread and relay
//   sessions      — borrows http & bus (references, not owners)
//   relay         — borrows channel & bus (references, not owners)
//   loop_thread   — must be joined before anything above is destroyed
//
// The destructor explicitly stops the thread and clears the bus first,
// so even if a caller deletes the handle without calling ptrclaw_destroy,
// we don't crash.

struct PtrClawHandle_ {
    ptrclaw::Config config;
    std::unique_ptr<ptrclaw::PlatformHttpClient> http;
    std::unique_ptr<ptrclaw::EmbedChannel> channel;
    std::unique_ptr<ptrclaw::EventBus> bus;
    std::unique_ptr<ptrclaw::SessionManager> sessions;
    std::unique_ptr<ptrclaw::StreamRelay> relay;
    std::thread loop_thread;
    std::atomic<bool> shutdown{false};

    // Per-session state: last response + optional stream callback.
    // Stream callbacks are set/cleared around ptrclaw_send_stream() calls;
    // the StreamChunkEvent handler forwards chunks to the active callback.
    struct SessionState {
        std::string last_response;
        std::function<void(const char*, int)> stream_cb;
    };
    std::mutex session_mutex;
    std::unordered_map<std::string, SessionState> session_state;

    std::string last_error;

    // Names of tools registered via ptrclaw_register_tool for this handle,
    // so we can unregister them on destroy.
    std::vector<std::string> bridged_tool_names;

    ~PtrClawHandle_() {
        // 1. Stop the background loop thread
        shutdown.store(true);
        if (loop_thread.joinable()) {
            loop_thread.join();
        }
        // 2. Clear event bus subscriptions — the lambdas capture `this`,
        //    so they must be removed before members they reference are destroyed.
        if (bus) {
            bus->clear();
        }
        // 3. Destroy sessions before bus/channel/http (releases Agents, Providers, Tools)
        sessions.reset();
        relay.reset();
        // 4. Unregister bridged tools from the global PluginRegistry so a
        //    subsequent create/destroy cycle doesn't see stale callbacks.
        auto& reg = ptrclaw::PluginRegistry::instance();
        for (const auto& name : bridged_tool_names) {
            reg.unregister_tool(name);
        }
    }

    PtrClawHandle_(const PtrClawHandle_&) = delete;
    PtrClawHandle_& operator=(const PtrClawHandle_&) = delete;
    PtrClawHandle_() = default;
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

// Track tool names registered for the current (not-yet-created) handle,
// so ptrclaw_destroy can unregister them from the global PluginRegistry.
static std::vector<std::string> g_pending_tool_names;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

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

    g_pending_tool_names.push_back(n);
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

        h->channel = std::make_unique<ptrclaw::EmbedChannel>();

        // Wire up EventBus + SessionManager + StreamRelay (same as run_channel)
        h->bus = std::make_unique<ptrclaw::EventBus>();
        h->sessions = std::make_unique<ptrclaw::SessionManager>(h->config, *h->http);
        h->sessions->set_event_bus(h->bus.get());

        h->relay = std::make_unique<ptrclaw::StreamRelay>(*h->channel, *h->bus);
        h->relay->subscribe_events();
        h->sessions->subscribe_events();

        // Forward streaming chunks to the active per-session callback
        ptrclaw::subscribe<ptrclaw::StreamChunkEvent>(*h->bus,
            [h](const ptrclaw::StreamChunkEvent& ev) {
                std::lock_guard<std::mutex> lock(h->session_mutex);
                auto it = h->session_state.find(ev.session_id);
                if (it != h->session_state.end() &&
                    it->second.stream_cb && !ev.delta.empty()) {
                    it->second.stream_cb(ev.delta.c_str(), 0);
                }
            });

        // Adopt pending bridged tool names so destroy can unregister them
        h->bridged_tool_names = std::move(g_pending_tool_names);
        g_pending_tool_names.clear();

        // Start background loop
        h->loop_thread = std::thread(embed_loop, h);

    } catch (const std::exception& e) {
        h->last_error = std::string("create failed: ") + e.what();
        // If tool adoption didn't happen yet, adopt pending names now
        // so the destructor can unregister them from PluginRegistry.
        if (h->bridged_tool_names.empty() && !g_pending_tool_names.empty()) {
            h->bridged_tool_names = std::move(g_pending_tool_names);
        }
        g_pending_tool_names.clear();
        delete h;
        return nullptr;
    }

    return h;
}

extern "C" void ptrclaw_destroy(PtrClawHandle handle) {
    if (!handle) return;
    delete handle;  // destructor handles thread, bus, sessions, tools
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

    try {
        std::string response = handle->channel->send_user_message(
            session_id, message);
        std::lock_guard<std::mutex> lock(handle->session_mutex);
        handle->session_state[session_id].last_response = std::move(response);
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
    std::lock_guard<std::mutex> lock(handle->session_mutex);
    auto it = handle->session_state.find(session_id);
    if (it != handle->session_state.end())
        return it->second.last_response.c_str();
    return empty;
}

extern "C" int ptrclaw_send_stream(PtrClawHandle handle,
                                    const char* session_id,
                                    const char* message,
                                    PtrClawChunkCallback on_chunk,
                                    void* userdata) {
    if (!handle || !session_id || !message || !on_chunk)
        return PTRCLAW_ERR_INVALID;

    try {
        // Install stream callback before sending so chunks are captured
        {
            std::lock_guard<std::mutex> lock(handle->session_mutex);
            handle->session_state[session_id].stream_cb =
                [on_chunk, userdata](const char* chunk, int done) {
                    on_chunk(chunk, done, userdata);
                };
        }

        std::string response = handle->channel->send_user_message(
            session_id, message);

        // Send terminal sentinel and clear callback
        on_chunk("", 1, userdata);

        {
            std::lock_guard<std::mutex> lock(handle->session_mutex);
            auto& state = handle->session_state[session_id];
            state.stream_cb = nullptr;
            state.last_response = std::move(response);
        }
        return PTRCLAW_OK;
    } catch (const std::exception& e) {
        // Clear callback on error
        {
            std::lock_guard<std::mutex> lock(handle->session_mutex);
            handle->session_state[session_id].stream_cb = nullptr;
        }
        handle->last_error = std::string("stream failed: ") + e.what();
        return PTRCLAW_ERR_PROVIDER;
    }
}
