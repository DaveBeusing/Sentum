#pragma once

#include <algorithm>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <sentum/api/model/Kline.hpp>

class MarketDataStore {
public:
    explicit MarketDataStore(std::size_t capacity_per_symbol = 600)
        : capacity_per_symbol_(capacity_per_symbol) {}

    static MarketDataStore& global() {
        static MarketDataStore instance;
        return instance;
    }

    void upsert(const std::string& symbol, const Kline& kline) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& buffer = buffers_[symbol];
        if (!buffer.empty() && buffer.back().timestamp == kline.timestamp) {
            buffer.back() = kline;
            return;
        }
        buffer.push_back(kline);
        while (buffer.size() > capacity_per_symbol_) buffer.pop_front();
    }

    std::vector<std::string> symbols() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> result;
        result.reserve(buffers_.size());
        for (const auto& entry : buffers_) result.push_back(entry.first);
        return result;
    }

    std::vector<Kline> latest(const std::string& symbol, std::size_t limit) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = buffers_.find(symbol);
        if (it == buffers_.end() || limit == 0) return {};
        const auto& buffer = it->second;
        const std::size_t count = std::min(limit, buffer.size());
        return std::vector<Kline>(buffer.end() - static_cast<std::ptrdiff_t>(count), buffer.end());
    }

private:
    std::size_t capacity_per_symbol_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::deque<Kline>> buffers_;
};
