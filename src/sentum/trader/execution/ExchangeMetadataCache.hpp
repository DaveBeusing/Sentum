#pragma once

#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>

#include <sentum/api/BinanceSpotExecutionClient.hpp>
#include <sentum/trader/execution/ExchangeRules.hpp>

namespace sentum::execution {

class ExchangeMetadataCache {
public:
    explicit ExchangeMetadataCache(const BinanceSpotExecutionClient& client,
                                   std::chrono::minutes ttl = std::chrono::minutes(60))
        : client_(client), ttl_(ttl) {}

    ExchangeRules rules(const std::string& symbol) const {
        const auto now = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = rules_.find(symbol);
            if (it != rules_.end() && now - it->second.loaded_at < ttl_) return it->second.rules;
        }
        auto loaded = ExchangeRules::from_exchange_info(client_.exchange_info(symbol), symbol);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            rules_[symbol] = {loaded, now};
        }
        return loaded;
    }

    void invalidate(const std::string& symbol) {
        std::lock_guard<std::mutex> lock(mutex_);
        rules_.erase(symbol);
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        rules_.clear();
    }

private:
    struct Entry { ExchangeRules rules; std::chrono::steady_clock::time_point loaded_at; };
    const BinanceSpotExecutionClient& client_;
    std::chrono::minutes ttl_;
    mutable std::mutex mutex_;
    mutable std::unordered_map<std::string, Entry> rules_;
};

} // namespace sentum::execution
