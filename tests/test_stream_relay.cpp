#include <catch2/catch_test_macros.hpp>
#include "stream_relay.hpp"
#include "event.hpp"
#include "event_bus.hpp"
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace ptrclaw;

namespace {

// Records what the relay asked the channel to do. Locked because with a worker
// pool the relay drives it from several threads at once — the contract stated on
// Channel: the send side must be thread-safe.
class RecordingChannel : public Channel {
public:
    std::string channel_name() const override { return "recording"; }
    bool health_check() override { return true; }
    bool supports_streaming_display() const override { return true; }

    void send_message(const std::string& target,
                      const std::string& message) override {
        std::lock_guard<std::mutex> lock(m_);
        sent_[target] = message;
    }

    void send_typing_indicator(const std::string& target) override {
        std::lock_guard<std::mutex> lock(m_);
        ++typing_[target];
    }

    int64_t send_streaming_placeholder(const std::string& target) override {
        std::lock_guard<std::mutex> lock(m_);
        return ++placeholder_id_[target] + 1000;
    }

    void edit_message(const std::string& target, int64_t /*message_id*/,
                      const std::string& content) override {
        std::lock_guard<std::mutex> lock(m_);
        edited_[target] = content;
    }

    std::string sent(const std::string& target) {
        std::lock_guard<std::mutex> lock(m_);
        return sent_.count(target) ? sent_[target] : "";
    }
    std::string edited(const std::string& target) {
        std::lock_guard<std::mutex> lock(m_);
        return edited_.count(target) ? edited_[target] : "";
    }

private:
    std::mutex m_;
    std::unordered_map<std::string, std::string> sent_;
    std::unordered_map<std::string, std::string> edited_;
    std::unordered_map<std::string, int> typing_;
    std::unordered_map<std::string, int64_t> placeholder_id_;
};

MessageReceivedEvent inbound(const std::string& session) {
    MessageReceivedEvent ev;
    ev.session_id = session;
    ev.message.sender = session;
    ev.message.content = "hello";
    ev.message.reply_target = session;
    return ev;
}

} // namespace

TEST_CASE("StreamRelay: delivers a non-streamed reply", "[stream_relay]") {
    EventBus bus;
    RecordingChannel channel;
    StreamRelay relay(channel, bus);
    relay.subscribe_events();

    bus.publish(inbound("s1"));

    MessageReadyEvent ready;
    ready.session_id = "s1";
    ready.reply_target = "s1";
    ready.content = "the answer";
    bus.publish(ready);

    REQUIRE(channel.sent("s1") == "the answer");
}

TEST_CASE("StreamRelay: a streamed reply is edited, not re-sent",
          "[stream_relay]") {
    EventBus bus;
    RecordingChannel channel;
    StreamRelay relay(channel, bus);
    relay.subscribe_events();

    bus.publish(inbound("s1"));

    StreamStartEvent start;
    start.session_id = "s1";
    bus.publish(start);

    StreamChunkEvent chunk;
    chunk.session_id = "s1";
    chunk.delta = "partial";
    bus.publish(chunk);

    StreamEndEvent end;
    end.session_id = "s1";
    bus.publish(end);

    REQUIRE(channel.edited("s1") == "partial");

    // Final content differs from what was streamed → edited again, not sent.
    MessageReadyEvent ready;
    ready.session_id = "s1";
    ready.reply_target = "s1";
    ready.content = "final";
    bus.publish(ready);

    REQUIRE(channel.edited("s1") == "final");
    REQUIRE(channel.sent("s1").empty());
}

TEST_CASE("StreamRelay: state is dropped once the reply is delivered",
          "[stream_relay]") {
    EventBus bus;
    RecordingChannel channel;
    StreamRelay relay(channel, bus);
    relay.subscribe_events();

    bus.publish(inbound("s1"));

    MessageReadyEvent ready;
    ready.session_id = "s1";
    ready.reply_target = "s1";
    ready.content = "first";
    bus.publish(ready);

    // A second MessageReady with no state must still be delivered directly.
    ready.content = "second";
    bus.publish(ready);
    REQUIRE(channel.sent("s1") == "second");
}

TEST_CASE("StreamRelay: concurrent sessions do not corrupt the state map",
          "[stream_relay]") {
    // One StreamRelay serves every session, so with a worker pool its handlers run
    // on all worker threads at once. Without the map lock this is an unordered_map
    // rehashing under concurrent insert — run under -Db_sanitize=thread to see it.
    EventBus bus;
    RecordingChannel channel;
    StreamRelay relay(channel, bus);
    relay.subscribe_events();

    constexpr int kThreads = 8;
    constexpr int kPerThread = 40;

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&bus, t] {
            for (int i = 0; i < kPerThread; ++i) {
                std::string session =
                    "s" + std::to_string(t) + "-" + std::to_string(i);

                bus.publish(inbound(session));

                StreamStartEvent start;
                start.session_id = session;
                bus.publish(start);

                StreamChunkEvent chunk;
                chunk.session_id = session;
                chunk.delta = session;
                bus.publish(chunk);

                StreamEndEvent end;
                end.session_id = session;
                bus.publish(end);

                MessageReadyEvent ready;
                ready.session_id = session;
                ready.reply_target = session;
                ready.content = session;
                bus.publish(ready);
            }
        });
    }
    for (auto& thread : threads) thread.join();

    // Every session's own content reached the channel — no crossed wires.
    for (int t = 0; t < kThreads; ++t) {
        for (int i = 0; i < kPerThread; ++i) {
            std::string session =
                "s" + std::to_string(t) + "-" + std::to_string(i);
            INFO("session: " << session);
            REQUIRE(channel.edited(session) == session);
        }
    }
}
