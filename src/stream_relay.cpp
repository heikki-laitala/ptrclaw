#include "stream_relay.hpp"
#include "event.hpp"
#include "event_bus.hpp"

namespace ptrclaw {

StreamRelay::StreamRelay(Channel& channel, EventBus& bus)
    : channel_(channel), bus_(bus)
{}

std::shared_ptr<StreamRelay::StreamState>
StreamRelay::find_state(const std::string& session_id) const {
    std::lock_guard<std::mutex> lock(states_mutex_);
    auto it = stream_states_.find(session_id);
    return it == stream_states_.end() ? nullptr : it->second;
}

std::shared_ptr<StreamRelay::StreamState>
StreamRelay::take_state(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(states_mutex_);
    auto it = stream_states_.find(session_id);
    if (it == stream_states_.end()) return nullptr;
    auto state = it->second;
    stream_states_.erase(it);
    return state;
}

void StreamRelay::subscribe_events() {
    // MessageReady → send via channel (skip if already delivered via streaming)
    ptrclaw::subscribe<MessageReadyEvent>(bus_,
        [this](const MessageReadyEvent& ev) {
            if (auto state = take_state(ev.session_id)) {
                if (state->delivered) {
                    // Content was replaced after streaming (e.g. soul extraction) —
                    // edit the streamed message with the final content
                    if (state->message_id != 0 && ev.content != state->accumulated) {
                        channel_.edit_message(state->chat_id, state->message_id,
                                              ev.content);
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
            auto state = std::make_shared<StreamState>();
            state->chat_id = chat_id;
            state->last_edit = std::chrono::steady_clock::now();
            {
                std::lock_guard<std::mutex> lock(states_mutex_);
                stream_states_[ev.session_id] = std::move(state);
            }
            channel_.send_typing_indicator(chat_id);
        });

    // Refresh typing indicator on each tool call
    ptrclaw::subscribe<ToolCallRequestEvent>(bus_,
        [this](const ToolCallRequestEvent& ev) {
            auto state = find_state(ev.session_id);
            if (state && !state->chat_id.empty()) {
                channel_.send_typing_indicator(state->chat_id);
            }
        });

    // Stream event subscribers (progressive message editing)
    if (!channel_.supports_streaming_display()) return;

    ptrclaw::subscribe<StreamStartEvent>(bus_,
        [this](const StreamStartEvent& ev) {
            auto state = find_state(ev.session_id);
            if (!state) return;
            state->message_id = channel_.send_streaming_placeholder(state->chat_id);
            state->last_edit = std::chrono::steady_clock::now();
        });

    ptrclaw::subscribe<StreamChunkEvent>(bus_,
        [this](const StreamChunkEvent& ev) {
            auto state = find_state(ev.session_id);
            if (!state || state->message_id == 0) return;
            state->accumulated += ev.delta;
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<
                std::chrono::milliseconds>(now - state->last_edit).count();
            if (elapsed >= 1000) {
                channel_.edit_message(state->chat_id, state->message_id,
                                      state->accumulated);
                state->last_edit = now;
            }
        });

    ptrclaw::subscribe<StreamEndEvent>(bus_,
        [this](const StreamEndEvent& ev) {
            auto state = find_state(ev.session_id);
            if (!state || state->message_id == 0) return;
            channel_.edit_message(state->chat_id, state->message_id,
                                  state->accumulated);
            state->delivered = true;
        });
}

} // namespace ptrclaw
