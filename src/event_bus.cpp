#include "event_bus.hpp"

namespace ptrclaw {

uint64_t EventBus::subscribe(const std::string& tag, EventHandler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t id = next_id_++;
    handlers_[tag].push_back(Subscription{id, std::move(handler)});
    return id;
}

bool EventBus::unsubscribe(uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [tag, subs] : handlers_) {
        for (auto it = subs.begin(); it != subs.end(); ++it) {
            if (it->id == id) {
                subs.erase(it);
                return true;
            }
        }
    }
    return false;
}

void EventBus::publish(const Event& event) {
    // Copy handlers out under lock, then call without lock held.
    std::vector<EventHandler> to_call;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = handlers_.find(event.type_tag);
        if (it == handlers_.end()) return;
        to_call.reserve(it->second.size());
        for (const auto& sub : it->second) {
            to_call.push_back(sub.handler);
        }
    }
    for (const auto& handler : to_call) {
        handler(event);
    }
}

void EventBus::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    handlers_.clear();
}

size_t EventBus::subscriber_count(const std::string& tag) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = handlers_.find(tag);
    if (it == handlers_.end()) return 0;
    return it->second.size();
}

} // namespace ptrclaw
