#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <shared_mutex>
#include <unordered_map>
#include <utility>

#include <sentum/market/MarketEvent.hpp>

namespace sentum::market {

class MarketEventBus {
public:
    using Handler = std::function<void(const MarketEvent&)>;
    using SubscriptionId = std::uint64_t;

    static MarketEventBus& global() {
        static MarketEventBus instance;
        return instance;
    }

    SubscriptionId subscribe(Handler handler) {
        const auto id = next_id_.fetch_add(1, std::memory_order_relaxed);
        std::unique_lock<std::shared_mutex> lock(mutex_);
        handlers_.emplace(id, std::move(handler));
        return id;
    }

    void unsubscribe(SubscriptionId id) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        handlers_.erase(id);
    }

    void publish(const MarketEvent& event) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        for (const auto& [_, handler] : handlers_) handler(event);
    }

private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<SubscriptionId, Handler> handlers_;
    std::atomic<SubscriptionId> next_id_{1};
};

} // namespace sentum::market
