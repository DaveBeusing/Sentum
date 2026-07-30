#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <sentum/utils/Database.hpp>
#include <sentum/utils/AsyncLogger.hpp>
#include <sentum/api/model/MarketInfo.hpp>
#include <sentum/market/MarketDataStore.hpp>

class Collector {
public:
    Collector(Database& db, MarketDataStore& store, const std::vector<MarketInfo>& markets);
    ~Collector();
    void start();
    void stop();

    std::uint64_t enqueued_count() const { return enqueued.load(); }
    std::uint64_t dropped_count() const { return dropped.load(); }
    double drop_rate() const;

private:
    struct Impl;
    void run();
    void writer_loop();
    bool try_enqueue(std::string symbol, Kline kline);

    static constexpr std::size_t queue_capacity = 8192;
    static constexpr std::size_t batch_size = 256;
    static constexpr double max_drop_rate = 0.001;

    Database& db_ref;
    MarketDataStore& store_ref;
    std::vector<MarketInfo> markets;
    std::thread ws_thread;
    std::thread writer_thread;
    std::atomic<bool> running{false};
    std::atomic<std::uint64_t> enqueued{0};
    std::atomic<std::uint64_t> dropped{0};
    std::mutex queue_mutex;
    std::condition_variable queue_cv;
    std::deque<std::pair<std::string, Kline>> queue;
    AsyncLogger logger;
    std::unique_ptr<Impl> impl;
};
