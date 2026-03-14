#include "../../include/ptrclaw/ptrclaw.h"
#include "../agent.hpp"
#include "../config.hpp"
#include "../event.hpp"
#include "../event_bus.hpp"
#include "../http.hpp"
#include "../plugin.hpp"
#include "../provider.hpp"
#include <nlohmann/json.hpp>
#include <memory>
#include <string>
#include <vector>

// ── Opaque handle implementations ────────────────────────────────

struct PtrClawEngine_ {
    std::string provider_name    = "anthropic";
    std::string api_key;
    std::string model            = "claude-sonnet-4-6";
    double      temperature      = 0.7;
    std::vector<std::string> tool_filter;  // empty + !filter_active → all tools
    bool        filter_active    = false;
    ptrclaw::AgentConfig agent_config;
    std::string last_error;
};

struct PtrClawSession_ {
    PtrClawEngine_*                          engine = nullptr;
    std::unique_ptr<ptrclaw::PlatformHttpClient> http;
    std::unique_ptr<ptrclaw::EventBus>       event_bus;
    std::unique_ptr<ptrclaw::Agent>          agent;
    std::string                              last_response;
};

// ── Version ──────────────────────────────────────────────────────

extern "C" const char *ptrclaw_version(void) {
    return PTRCLAW_VERSION_STRING;
}

// ── Engine lifecycle ─────────────────────────────────────────────

extern "C" PtrClawEngine ptrclaw_engine_create(const char *config_json) {
    if (!config_json) return nullptr;

    auto *eng = new PtrClawEngine_();
    try {
        auto j = nlohmann::json::parse(config_json);

        if (j.contains("provider") && j["provider"].is_string())
            eng->provider_name = j["provider"].get<std::string>();
        if (j.contains("api_key") && j["api_key"].is_string())
            eng->api_key = j["api_key"].get<std::string>();
        if (j.contains("model") && j["model"].is_string())
            eng->model = j["model"].get<std::string>();
        if (j.contains("temperature") && j["temperature"].is_number())
            eng->temperature = j["temperature"].get<double>();
        if (j.contains("max_tool_iterations") && j["max_tool_iterations"].is_number_unsigned())
            eng->agent_config.max_tool_iterations = j["max_tool_iterations"].get<uint32_t>();
        if (j.contains("max_history_messages") && j["max_history_messages"].is_number_unsigned())
            eng->agent_config.max_history_messages = j["max_history_messages"].get<uint32_t>();

        if (j.contains("tools") && j["tools"].is_array()) {
            eng->filter_active = true;
            for (const auto &t : j["tools"]) {
                if (t.is_string())
                    eng->tool_filter.push_back(t.get<std::string>());
            }
        }
    } catch (const std::exception &e) {
        eng->last_error = std::string("config parse error: ") + e.what();
    }
    return eng;
}

extern "C" void ptrclaw_engine_destroy(PtrClawEngine engine) {
    delete engine;
}

extern "C" const char *ptrclaw_engine_last_error(PtrClawEngine engine) {
    if (!engine) return "null engine";
    return engine->last_error.c_str();
}

// ── Session lifecycle ────────────────────────────────────────────

extern "C" PtrClawSession ptrclaw_session_create(PtrClawEngine engine,
                                                  const char *session_id) {
    if (!engine) return nullptr;

    auto *sess = new PtrClawSession_();
    sess->engine = engine;

    try {
        sess->http = std::make_unique<ptrclaw::PlatformHttpClient>();

        // Build a minimal Config from engine settings.
        ptrclaw::Config cfg;
        cfg.provider    = engine->provider_name;
        cfg.model       = engine->model;
        cfg.temperature = engine->temperature;
        cfg.agent       = engine->agent_config;
        cfg.memory.backend = "none";  // no persistent memory by default
        cfg.providers[engine->provider_name].api_key = engine->api_key;

        auto provider = ptrclaw::create_provider(
            engine->provider_name,
            engine->api_key,
            *sess->http,
            /*base_url=*/"",
            /*prompt_caching=*/false,
            /*provider_entry=*/nullptr);

        if (!provider) {
            engine->last_error = "unknown provider: " + engine->provider_name;
            delete sess;
            return nullptr;
        }

        // Collect tools, optionally filtered to the host-supplied list.
        auto all_tools = ptrclaw::PluginRegistry::instance().create_all_tools();
        std::vector<std::unique_ptr<ptrclaw::Tool>> tools;

        if (!engine->filter_active) {
            tools = std::move(all_tools);
        } else {
            for (auto &t : all_tools) {
                for (const auto &name : engine->tool_filter) {
                    if (name == t->tool_name()) {
                        tools.push_back(std::move(t));
                        break;
                    }
                }
            }
        }

        sess->agent = std::make_unique<ptrclaw::Agent>(
            std::move(provider), std::move(tools), cfg);

        if (session_id && *session_id)
            sess->agent->set_session_id(session_id);

        // Attach an EventBus so that streaming providers can emit chunk events.
        sess->event_bus = std::make_unique<ptrclaw::EventBus>();
        sess->agent->set_event_bus(sess->event_bus.get());

    } catch (const std::exception &e) {
        engine->last_error = std::string("session create failed: ") + e.what();
        delete sess;
        return nullptr;
    }
    return sess;
}

extern "C" void ptrclaw_session_destroy(PtrClawSession session) {
    delete session;
}

extern "C" void ptrclaw_session_clear_history(PtrClawSession session) {
    if (session && session->agent)
        session->agent->clear_history();
}

// ── Message processing ───────────────────────────────────────────

extern "C" int ptrclaw_process(PtrClawSession session, const char *message) {
    if (!session || !message || !session->agent) return PTRCLAW_ERR_INVALID;
    try {
        session->last_response = session->agent->process(message);
        return PTRCLAW_OK;
    } catch (const std::exception &e) {
        if (session->engine)
            session->engine->last_error = std::string("process failed: ") + e.what();
        return PTRCLAW_ERR_PROVIDER;
    }
}

extern "C" const char *ptrclaw_session_last_response(PtrClawSession session) {
    if (!session) return nullptr;
    return session->last_response.c_str();
}

extern "C" int ptrclaw_process_stream(PtrClawSession session,
                                       const char *message,
                                       PtrClawChunkCallback on_chunk,
                                       void *userdata) {
    if (!session || !message || !on_chunk) return PTRCLAW_ERR_INVALID;
    if (!session->agent || !session->event_bus)  return PTRCLAW_ERR_INVALID;

    // Subscribe to stream-chunk events emitted by the provider during chat_stream().
    bool any_chunks = false;
    auto sub_id = ptrclaw::subscribe<ptrclaw::StreamChunkEvent>(
        *session->event_bus,
        std::function<void(const ptrclaw::StreamChunkEvent &)>(
            [on_chunk, userdata, &any_chunks](const ptrclaw::StreamChunkEvent &ev) {
                if (!ev.delta.empty()) {
                    on_chunk(ev.delta.c_str(), 0, userdata);
                    any_chunks = true;
                }
            }));

    try {
        session->last_response = session->agent->process(message);
    } catch (const std::exception &e) {
        session->event_bus->unsubscribe(sub_id);
        if (session->engine)
            session->engine->last_error =
                std::string("process_stream failed: ") + e.what();
        return PTRCLAW_ERR_PROVIDER;
    }

    session->event_bus->unsubscribe(sub_id);

    // Non-streaming fallback: emit the full response as one chunk.
    if (!any_chunks && !session->last_response.empty())
        on_chunk(session->last_response.c_str(), 0, userdata);

    // Terminal sentinel.
    on_chunk("", 1, userdata);
    return PTRCLAW_OK;
}
