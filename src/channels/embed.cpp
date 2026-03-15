#include "embed.hpp"
#include "../plugin.hpp"
#include <chrono>

static ptrclaw::ChannelRegistrar reg_embed("embed",
    [](const ptrclaw::Config&, ptrclaw::HttpClient&) {
        return std::make_unique<ptrclaw::EmbedChannel>();
    });

namespace ptrclaw {

EmbedChannel::EmbedChannel() = default;
EmbedChannel::~EmbedChannel() = default;

// ── Channel interface ────────────────────────────────────────────

void EmbedChannel::send_message(const std::string& target,
                                 const std::string& message) {
    // target is the session_id (set as reply_target in send_user_message).
    // Deliver the response to the waiting host thread.
    std::lock_guard<std::mutex> lock(response_mutex_);
    auto& pending = pending_responses_[target];
    pending.content = message;
    pending.ready = true;
    response_cv_.notify_all();
}

std::vector<ChannelMessage> EmbedChannel::poll_updates() {
    std::unique_lock<std::mutex> lock(inbound_mutex_);
    if (inbound_queue_.empty()) {
        inbound_cv_.wait_for(lock, std::chrono::milliseconds(100));
    }
    std::vector<ChannelMessage> result;
    result.swap(inbound_queue_);
    return result;
}

// ── Host API ─────────────────────────────────────────────────────

std::string EmbedChannel::send_user_message(const std::string& session_id,
                                             const std::string& message) {
    // Prepare response slot
    {
        std::lock_guard<std::mutex> lock(response_mutex_);
        pending_responses_[session_id] = PendingResponse{};
    }

    // Push message into the inbound queue
    {
        std::lock_guard<std::mutex> lock(inbound_mutex_);
        ChannelMessage cm;
        cm.sender = session_id;
        cm.content = message;
        cm.channel = "embed";
        cm.reply_target = session_id;
        inbound_queue_.push_back(std::move(cm));
        inbound_cv_.notify_one();
    }

    // Block until response is ready
    std::unique_lock<std::mutex> lock(response_mutex_);
    response_cv_.wait(lock, [&] {
        auto it = pending_responses_.find(session_id);
        return it != pending_responses_.end() && it->second.ready;
    });

    std::string response = std::move(pending_responses_[session_id].content);
    pending_responses_.erase(session_id);
    return response;
}

std::string EmbedChannel::send_user_message_stream(
    const std::string& session_id,
    const std::string& message,
    EmbedChunkCallback on_chunk) {

    set_stream_callback(session_id, std::move(on_chunk));
    std::string response = send_user_message(session_id, message);
    clear_stream_callback(session_id);

    return response;
}

void EmbedChannel::set_stream_callback(const std::string& session_id,
                                        EmbedChunkCallback callback) {
    std::lock_guard<std::mutex> lock(stream_mutex_);
    stream_callbacks_[session_id] = std::move(callback);
}

void EmbedChannel::clear_stream_callback(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(stream_mutex_);
    stream_callbacks_.erase(session_id);
}

EmbedChunkCallback EmbedChannel::get_stream_callback(
    const std::string& session_id) {
    std::lock_guard<std::mutex> lock(stream_mutex_);
    auto it = stream_callbacks_.find(session_id);
    if (it != stream_callbacks_.end()) return it->second;
    return nullptr;
}

} // namespace ptrclaw
