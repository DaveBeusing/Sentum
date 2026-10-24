#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include <sentum/api/BinanceSpotExecutionClient.hpp>
#include <sentum/trader/execution/ExchangeRules.hpp>

namespace sentum::execution {

class ExchangeMetadataCache {
public:
    explicit ExchangeMetadataCache(const BinanceSpotExecutionClient& client,
                                   std::chrono::minutes ttl = std::chrono::minutes(60))
        : client_(client), ttl_(ttl) {
        if (ttl_ <= std::chrono::minutes::zero()) throw std::invalid_argument("Metadata cache TTL must be positive");
    }

    ExchangeRules rules(const std::string& symbol, bool refresh = false) const {
        const auto now = std::chrono::steady_clock::now();
        std::uint64_t generation;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (refresh) rules_.erase(symbol);
            auto it = rules_.find(symbol);
            if (it != rules_.end() && now - it->second.loaded_at < ttl_) return it->second.rules;
            rules_.erase(symbol);
            generation = ++generation_;
        }
        try {
            auto loaded = ExchangeRules::from_exchange_info(client_.exchange_info(symbol), symbol);
            std::lock_guard<std::mutex> lock(mutex_);
            if (generation != generation_) throw std::runtime_error("Exchange metadata invalidated while loading");
            rules_.insert_or_assign(symbol, Entry{loaded, std::chrono::steady_clock::now()});
            return loaded;
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (generation == generation_) {
                rules_.erase(symbol);
                ++generation_;
            }
            throw;
        }
    }

    void invalidate(const std::string& symbol) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++generation_;
        rules_.erase(symbol);
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        ++generation_;
        rules_.clear();
    }

private:
    struct Entry { ExchangeRules rules; std::chrono::steady_clock::time_point loaded_at; };
    const BinanceSpotExecutionClient& client_;
    std::chrono::minutes ttl_;
    mutable std::mutex mutex_;
    mutable std::uint64_t generation_ = 0;
    mutable std::unordered_map<std::string, Entry> rules_;
};

} // namespace sentum::execution
