#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <sentum/utils/Database.hpp>
#include <sentum/utils/AsyncLogger.hpp>
#include <sentum/api/model/MarketInfo.hpp>
#include <sentum/market/MarketDataStore.hpp>
#include <sentum/market/SpscRingQueue.hpp>
#include <sentum/market/SymbolId.hpp>

class Collector {
public:
    Collector(Database& db, const std::vector<MarketInfo>& markets);
    Collector(Database& db, MarketDataStore& store, const std::vector<MarketInfo>& markets);
    ~Collector();
    void start();
    void stop();

    std::uint64_t enqueued_count() const { return enqueued.load(std::memory_order_relaxed); }
    std::uint64_t dropped_count() const { return dropped.load(std::memory_order_relaxed); }
    double drop_rate() const;
    std::size_t queue_depth() const { return queue.size_approx(); }

private:
    struct Impl;
    struct SymbolRef {
        sentum::market::SymbolId id = sentum::market::kInvalidSymbolId;
        const std::string* canonical = nullptr;
    };

    void run();
    void writer_loop();
    bool try_enqueue(const std::string* symbol, Kline kline);
    SymbolRef resolve_symbol(std::string_view symbol) const noexcept;
    void initialize_symbols();

    static constexpr std::size_t queue_capacity = 8192;
    static constexpr std::size_t batch_size = 256;
    static constexpr double max_drop_rate = 0.001;

    Database& db_ref;
    MarketDataStore& store_ref;
    std::vector<MarketInfo> markets;
    std::vector<std::string> canonical_symbols;
    std::unordered_map<std::uint64_t, std::size_t> symbol_by_hash;
    std::thread ws_thread;
    std::thread writer_thread;
    std::atomic<bool> running{false};
    std::atomic<std::uint64_t> enqueued{0};
    std::atomic<std::uint64_t> dropped{0};
    sentum::market::SpscRingQueue<KlineBatchItem, queue_capacity + 1> queue;
    std::mutex wait_mutex;
    std::condition_variable queue_cv;
    AsyncLogger logger;
    std::unique_ptr<Impl> impl;
};
