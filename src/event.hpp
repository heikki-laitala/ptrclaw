#pragma once
#include "provider.hpp"
#include "channel.hpp"
#include <string>
#include <cstdint>

namespace ptrclaw {

// Tag-based event dispatch — no RTTI, no dynamic_cast.
// Events are stack-allocated structs; never deleted through base pointer.

struct Event {
    const char* type_tag;
};

// ── Event tags ──────────────────────────────────────────────────

namespace event_tags {
    constexpr const char* MessageReceived  = "MessageReceived";
    constexpr const char* MessageReady     = "MessageReady";
    constexpr const char* ProviderRequest  = "ProviderRequest";
    constexpr const char* ProviderResponse = "ProviderResponse";
    constexpr const char* ToolCallRequest  = "ToolCallRequest";
    constexpr const char* ToolCallResult   = "ToolCallResult";
    constexpr const char* SessionCreated   = "SessionCreated";
    constexpr const char* SessionEvicted   = "SessionEvicted";
    constexpr const char* StreamStart      = "StreamStart";
    constexpr const char* StreamChunk      = "StreamChunk";
    constexpr const char* StreamEnd        = "StreamEnd";
} // namespace event_tags

// ── Event structs ───────────────────────────────────────────────

struct MessageReceivedEvent : Event {
    static constexpr const char* TAG = event_tags::MessageReceived;
    std::string session_id;
    ChannelMessage message;

    MessageReceivedEvent() { type_tag = TAG; }
};

struct MessageReadyEvent : Event {
    static constexpr const char* TAG = event_tags::MessageReady;
    std::string session_id;
    std::string reply_target;
    std::string content;

    MessageReadyEvent() { type_tag = TAG; }
};

struct ProviderRequestEvent : Event {
    static constexpr const char* TAG = event_tags::ProviderRequest;
    std::string session_id;
    std::string model;
    size_t message_count = 0;
    size_t tool_count = 0;

    ProviderRequestEvent() { type_tag = TAG; }
};

struct ProviderResponseEvent : Event {
    static constexpr const char* TAG = event_tags::ProviderResponse;
    std::string session_id;
    std::string model;
    bool has_tool_calls = false;
    TokenUsage usage;

    ProviderResponseEvent() { type_tag = TAG; }
};

struct ToolCallRequestEvent : Event {
    static constexpr const char* TAG = event_tags::ToolCallRequest;
    std::string session_id;
    std::string tool_name;
    std::string tool_call_id;

    ToolCallRequestEvent() { type_tag = TAG; }
};

struct ToolCallResultEvent : Event {
    static constexpr const char* TAG = event_tags::ToolCallResult;
    std::string session_id;
    std::string tool_name;
    bool success = false;

    ToolCallResultEvent() { type_tag = TAG; }
};

struct SessionCreatedEvent : Event {
    static constexpr const char* TAG = event_tags::SessionCreated;
    std::string session_id;

    SessionCreatedEvent() { type_tag = TAG; }
};

struct SessionEvictedEvent : Event {
    static constexpr const char* TAG = event_tags::SessionEvicted;
    std::string session_id;

    SessionEvictedEvent() { type_tag = TAG; }
};

struct StreamStartEvent : Event {
    static constexpr const char* TAG = event_tags::StreamStart;
    std::string session_id;
    std::string model;

    StreamStartEvent() { type_tag = TAG; }
};

struct StreamChunkEvent : Event {
    static constexpr const char* TAG = event_tags::StreamChunk;
    std::string session_id;
    std::string delta;

    StreamChunkEvent() { type_tag = TAG; }
};

struct StreamEndEvent : Event {
    static constexpr const char* TAG = event_tags::StreamEnd;
    std::string session_id;

    StreamEndEvent() { type_tag = TAG; }
};

} // namespace ptrclaw
