#include "stream_relay.hpp"
#include "event.hpp"
#include "event_bus.hpp"

namespace ptrclaw {

StreamRelay::StreamRelay(Channel& channel, EventBus& bus)
    : channel_(channel), bus_(bus)
{}

void StreamRelay::subscribe_events() {
    // MessageReady → send via channel (skip if already delivered via streaming)
    ptrclaw::subscribe<MessageReadyEvent>(bus_,
        [this](const MessageReadyEvent& ev) {
            auto it = stream_states_.find(ev.session_id);
            if (it != stream_states_.end()) {
                bool delivered = it->second.delivered;
                auto chat_id = it->second.chat_id;
                auto msg_id = it->second.message_id;
                auto accumulated = it->second.accumulated;
                stream_states_.erase(it);
                if (delivered) {
                    // Content was replaced after streaming (e.g. soul extraction) —
                    // edit the streamed message with the final content
                    if (msg_id != 0 && ev.content != accumulated) {
                        channel_.edit_message(chat_id, msg_id, ev.content);
                    }
                    return;
                }
            }
            if (!ev.reply_target.empty()) {
                channel_.send_message(ev.reply_target, ev.content);
            }
        });

    // MessageReceived → typing indicator + stream state (skip commands)
    ptrclaw::subscribe<MessageReceivedEvent>(bus_,
        [this](const MessageReceivedEvent& ev) {
            if (!ev.message.content.empty() && ev.message.content[0] == '/') return;

            std::string chat_id = ev.message.reply_target.value_or("");
            channel_.send_typing_indicator(chat_id);
            stream_states_[ev.session_id] = {chat_id, 0, {},
                                              std::chrono::steady_clock::now(), false};
        });

    // Refresh typing indicator on each tool call
    ptrclaw::subscribe<ToolCallRequestEvent>(bus_,
        [this](const ToolCallRequestEvent& ev) {
            auto it = stream_states_.find(ev.session_id);
            if (it != stream_states_.end() && !it->second.chat_id.empty()) {
                channel_.send_typing_indicator(it->second.chat_id);
            }
        });

    // Stream event subscribers (progressive message editing)
    if (!channel_.supports_streaming_display()) return;

    ptrclaw::subscribe<StreamStartEvent>(bus_,
        [this](const StreamStartEvent& ev) {
            auto it = stream_states_.find(ev.session_id);
            if (it == stream_states_.end()) return;
            int64_t msg_id = channel_.send_streaming_placeholder(
                it->second.chat_id);
            it->second.message_id = msg_id;
            it->second.last_edit = std::chrono::steady_clock::now();
        });

    ptrclaw::subscribe<StreamChunkEvent>(bus_,
        [this](const StreamChunkEvent& ev) {
            auto it = stream_states_.find(ev.session_id);
            if (it == stream_states_.end() || it->second.message_id == 0)
                return;
            it->second.accumulated += ev.delta;
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<
                std::chrono::milliseconds>(
                now - it->second.last_edit).count();
            if (elapsed >= 1000) {
                channel_.edit_message(it->second.chat_id,
                                      it->second.message_id,
                                      it->second.accumulated);
                it->second.last_edit = now;
            }
        });

    ptrclaw::subscribe<StreamEndEvent>(bus_,
        [this](const StreamEndEvent& ev) {
            auto it = stream_states_.find(ev.session_id);
            if (it == stream_states_.end() || it->second.message_id == 0)
                return;
            channel_.edit_message(it->second.chat_id,
                                  it->second.message_id,
                                  it->second.accumulated);
            it->second.delivered = true;
        });
}

} // namespace ptrclaw
