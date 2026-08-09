#pragma once

#include <algorithm>
#include <cstddef>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <sentum/api/model/Kline.hpp>
#include <sentum/market/SymbolId.hpp>

class MarketDataStore {
public:
    explicit MarketDataStore(std::size_t capacity_per_symbol = 600)
        : capacity_per_symbol_(capacity_per_symbol) {}

    static MarketDataStore& global() {
        static MarketDataStore instance;
        return instance;
    }

    void register_symbol(sentum::market::SymbolId id, const std::string& symbol) {
        if (id == sentum::market::kInvalidSymbolId) return;
        auto buffer = get_or_create(symbol);
        std::unique_lock<std::shared_mutex> lock(map_mutex_);
        if (id_buffers_.size() <= id) id_buffers_.resize(static_cast<std::size_t>(id) + 1);
        id_buffers_[id] = std::move(buffer);
    }

    void upsert(const std::string& symbol, const Kline& kline) { upsert_buffer(get_or_create(symbol), kline); }
    void upsert(sentum::market::SymbolId id, const Kline& kline) {
        auto buffer = find(id);
        if (buffer) upsert_buffer(std::move(buffer), kline);
    }

    std::vector<std::string> symbols() const {
        std::shared_lock<std::shared_mutex> lock(map_mutex_);
        std::vector<std::string> result;
        result.reserve(buffers_.size());
        for (const auto& entry : buffers_) result.push_back(entry.first);
        return result;
    }

    std::vector<Kline> latest(const std::string& symbol, std::size_t limit) const { return latest_buffer(find(symbol), limit); }
    std::vector<Kline> latest(sentum::market::SymbolId id, std::size_t limit) const { return latest_buffer(find(id), limit); }

    bool cumulative_return(const std::string& symbol, std::size_t lookback, double& result) const { return cumulative_return_buffer(find(symbol), lookback, result); }
    bool cumulative_return(sentum::market::SymbolId id, std::size_t lookback, double& result) const { return cumulative_return_buffer(find(id), lookback, result); }

    std::size_t size(const std::string& symbol) const { return buffer_size(find(symbol)); }
    std::size_t size(sentum::market::SymbolId id) const { return buffer_size(find(id)); }

private:
    struct RingBuffer {
        explicit RingBuffer(std::size_t c) : data(c), capacity(c) {}
        std::vector<Kline> data;
        const std::size_t capacity;
        std::size_t head = 0;
        std::size_t size = 0;
        mutable std::mutex mutex;
    };

    static void upsert_buffer(const std::shared_ptr<RingBuffer>& buffer, const Kline& kline) {
        if (!buffer) return;
        std::lock_guard<std::mutex> lock(buffer->mutex);
        if (buffer->size > 0) {
            const auto last_index = (buffer->head + buffer->capacity - 1) % buffer->capacity;
            if (buffer->data[last_index].timestamp == kline.timestamp) { buffer->data[last_index] = kline; return; }
        }
        buffer->data[buffer->head] = kline;
        buffer->head = (buffer->head + 1) % buffer->capacity;
        if (buffer->size < buffer->capacity) ++buffer->size;
    }

    static std::vector<Kline> latest_buffer(const std::shared_ptr<RingBuffer>& buffer, std::size_t limit) {
        if (!buffer || limit == 0) return {};
        std::lock_guard<std::mutex> lock(buffer->mutex);
        const std::size_t count = std::min(limit, buffer->size);
        std::vector<Kline> result; result.reserve(count);
        const std::size_t start = (buffer->head + buffer->capacity - count) % buffer->capacity;
        for (std::size_t i = 0; i < count; ++i) result.push_back(buffer->data[(start + i) % buffer->capacity]);
        return result;
    }

    static bool cumulative_return_buffer(const std::shared_ptr<RingBuffer>& buffer, std::size_t lookback, double& result) {
        if (!buffer || lookback < 2) return false;
        std::lock_guard<std::mutex> lock(buffer->mutex);
        const std::size_t count = std::min(lookback, buffer->size);
        if (count < 2) return false;
        const std::size_t first_index = (buffer->head + buffer->capacity - count) % buffer->capacity;
        const std::size_t last_index = (buffer->head + buffer->capacity - 1) % buffer->capacity;
        const double first = buffer->data[first_index].close;
        if (first <= 0.0) return false;
        result = (buffer->data[last_index].close - first) / first;
        return true;
    }

    static std::size_t buffer_size(const std::shared_ptr<RingBuffer>& buffer) {
        if (!buffer) return 0;
        std::lock_guard<std::mutex> lock(buffer->mutex);
        return buffer->size;
    }

    std::shared_ptr<RingBuffer> get_or_create(const std::string& symbol) {
        {
            std::shared_lock<std::shared_mutex> lock(map_mutex_);
            auto it = buffers_.find(symbol);
            if (it != buffers_.end()) return it->second;
        }
        std::unique_lock<std::shared_mutex> lock(map_mutex_);
        auto [it, inserted] = buffers_.emplace(symbol, nullptr);
        if (inserted) it->second = std::make_shared<RingBuffer>(capacity_per_symbol_);
        return it->second;
    }

    std::shared_ptr<RingBuffer> find(const std::string& symbol) const {
        std::shared_lock<std::shared_mutex> lock(map_mutex_);
        auto it = buffers_.find(symbol);
        return it == buffers_.end() ? nullptr : it->second;
    }

    std::shared_ptr<RingBuffer> find(sentum::market::SymbolId id) const {
        std::shared_lock<std::shared_mutex> lock(map_mutex_);
        const auto index = static_cast<std::size_t>(id);
        return index < id_buffers_.size() ? id_buffers_[index] : nullptr;
    }

    std::size_t capacity_per_symbol_;
    mutable std::shared_mutex map_mutex_;
    std::unordered_map<std::string, std::shared_ptr<RingBuffer>> buffers_;
    std::vector<std::shared_ptr<RingBuffer>> id_buffers_;
};
