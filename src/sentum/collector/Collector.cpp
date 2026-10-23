#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <stdexcept>

#include <sentum/collector/Collector.hpp>
#include <sentum/collector/FastBinanceKlineParser.hpp>
#include <sentum/market/MarketEventBus.hpp>
#include <sentum/market/RuntimePerformanceMetrics.hpp>
#include <sentum/utils/helper.hpp>

struct Collector::Impl {
    std::mutex mutex;
    std::shared_ptr<sentum::api::ReconnectingMarketStream> stream;
};

Collector::Collector(Database& db, const std::vector<MarketInfo>& markets_)
    : Collector(db, MarketDataStore::global(), markets_) {}

Collector::Collector(Database& db, MarketDataStore& store, const std::vector<MarketInfo>& markets_,
                     std::chrono::milliseconds maximum_event_age)
    : db_ref(db), store_ref(store), markets(markets_), logger("log/collector.log"), impl(std::make_unique<Impl>()) {
    if (maximum_event_age.count() <= 0 || maximum_event_age > std::chrono::hours(24)) {
        throw std::invalid_argument("Market event age must be positive and no greater than 24 hours");
    }
    stream_settings.maximum_event_age = maximum_event_age;
    stream_settings.idle_reconnect_timeout = std::max(std::chrono::milliseconds(10000), maximum_event_age * 5);
    initialize_symbols();
}

Collector::~Collector() { stop(); }

void Collector::initialize_symbols() {
    canonical_symbols.reserve(markets.size());
    symbol_by_hash.reserve(markets.size() * 2);
    for (std::size_t i = 0; i < markets.size(); ++i) {
        canonical_symbols.push_back(helper::to_lowercase(markets[i].symbol));
        const auto id = static_cast<sentum::market::SymbolId>(i + 1);
        symbol_by_hash.emplace(sentum::market::symbol_hash(markets[i].symbol), i);
        store_ref.register_symbol(id, canonical_symbols.back());
    }
}

Collector::SymbolRef Collector::resolve_symbol(std::string_view symbol) const noexcept {
    const auto it = symbol_by_hash.find(sentum::market::symbol_hash(symbol));
    if (it == symbol_by_hash.end()) return {};
    const auto index = it->second;
    if (index >= canonical_symbols.size()) return {};
    const auto& canonical = canonical_symbols[index];
    if (canonical.size() != symbol.size()) return {};
    for (std::size_t i = 0; i < symbol.size(); ++i) {
        char c = symbol[i]; if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (canonical[i] != c) return {};
    }
    return {static_cast<sentum::market::SymbolId>(index + 1), &canonical};
}

double Collector::drop_rate() const {
    const auto accepted = enqueued.load(std::memory_order_relaxed);
    const auto rejected = dropped.load(std::memory_order_relaxed);
    const auto total = accepted + rejected;
    return total == 0 ? 0.0 : static_cast<double>(rejected) / static_cast<double>(total);
}

bool Collector::persistence_failed() const noexcept {
    return persistence_state.load(std::memory_order_acquire) == PersistenceState::Failed;
}

bool Collector::persistence_healthy() const noexcept {
    return persistence_state.load(std::memory_order_acquire) == PersistenceState::Healthy;
}

nlohmann::json Collector::persistence_snapshot() const {
    std::lock_guard<std::mutex> lock(persistence_mutex);
    const auto state = persistence_state.load(std::memory_order_acquire);
    const auto committed = persisted.load(std::memory_order_acquire);
    // Queue publication can precede the producer's diagnostic increment. Every
    // committed item was admitted, even when that counter update is still pending.
    const auto accepted = std::max(enqueued.load(std::memory_order_acquire), committed);
    const auto unpersisted = accepted > committed ? accepted - committed : 0;
    const bool writer_active = persistence_writer_active.load(std::memory_order_acquire);
    return {
        {"status", state == PersistenceState::Failed ? "FAILED" : state == PersistenceState::Retrying ? "RETRYING" : "HEALTHY"},
        {"accepted", accepted}, {"persisted", committed}, {"unpersisted", unpersisted},
        {"unrecovered", state == PersistenceState::Failed ? unpersisted : 0},
        {"write_failures", persistence_write_failures.load(std::memory_order_relaxed)},
        {"retries", persistence_retries.load(std::memory_order_relaxed)},
        {"rejected_after_failure", persistence_rejected.load(std::memory_order_relaxed)},
        {"queue_overflow_drops", dropped.load(std::memory_order_relaxed)},
        {"last_sqlite_code", persistence_sqlite_code}, {"last_rollback_code", persistence_rollback_code},
        {"failure_reason", persistence_failure_reason}, {"writer_active", writer_active},
        {"drained", state == PersistenceState::Healthy && unpersisted == 0 && !writer_active &&
                    !running.load(std::memory_order_acquire) && producer_stopped.load(std::memory_order_acquire)}
    };
}

void Collector::rethrow_persistence_failure() const {
    if (!persistence_failed()) return;
    const auto snapshot = persistence_snapshot();
    throw std::runtime_error("Collector persistence failed: " + snapshot.at("failure_reason").get<std::string>() +
                             "; sqlite_code=" + std::to_string(snapshot.at("last_sqlite_code").get<int>()) +
                             "; rollback_code=" + std::to_string(snapshot.at("last_rollback_code").get<int>()) +
                             "; accepted=" + std::to_string(snapshot.at("accepted").get<std::uint64_t>()) +
                             "; unrecovered=" + std::to_string(snapshot.at("unrecovered").get<std::uint64_t>()));
}

void Collector::fail_persistence(int sqlite_code, int rollback_code, const char* reason) noexcept {
    {
        std::lock_guard<std::mutex> lock(persistence_mutex);
        persistence_sqlite_code = sqlite_code;
        persistence_rollback_code = rollback_code;
        persistence_failure_reason = reason;
        persistence_state.store(PersistenceState::Failed, std::memory_order_release);
    }
    running.store(false, std::memory_order_release);
    try { request_transport_stop(); } catch (...) {}
    queue_cv.notify_all();
    try { logger.log("Collector persistence failed: " + persistence_snapshot().dump()); } catch (...) {}
}

void Collector::update_queue_pressure(std::size_t depth, bool saturated) noexcept {
    auto& perf = sentum::market::RuntimePerformanceMetrics::global();
    perf.observe_queue_depth(depth);
    if (saturated) perf.observe_queue_saturation();

    const auto level = saturated
        ? sentum::market::QueuePressureLevel::Saturated
        : depth >= critical_pressure_depth
            ? sentum::market::QueuePressureLevel::Critical
            : depth >= elevated_pressure_depth
                ? sentum::market::QueuePressureLevel::Elevated
                : sentum::market::QueuePressureLevel::Normal;

    const auto raw = static_cast<std::uint8_t>(level);
    auto previous = queue_pressure_level_.load(std::memory_order_relaxed);
    if (previous != raw && queue_pressure_level_.compare_exchange_strong(
            previous, raw, std::memory_order_relaxed, std::memory_order_relaxed)) {
        perf.set_queue_pressure(level);
    }
}

sentum::api::MarketStreamHealth Collector::stream_health() const {
    std::shared_ptr<sentum::api::ReconnectingMarketStream> stream;
    { std::lock_guard<std::mutex> lock(impl->mutex); stream = impl->stream; }
    return stream ? stream->health() : sentum::api::MarketStreamHealth{};
}

void Collector::start() {
    rethrow_persistence_failure();
    std::unique_lock<std::mutex> lock(lifecycle_mutex);
    if (ws_worker_id == std::this_thread::get_id()) {
        if (running.load()) return;
        throw std::logic_error("A collector worker cannot restart itself before joining");
    }
    lifecycle_cv.wait(lock, [this] { return !joining; });
    if (running.load()) return;
    join_workers(lock);
    // A writer being joined may have latched a terminal failure while this
    // start waited for another lifecycle operation to complete.
    rethrow_persistence_failure();
    const std::string hostname = test_hostname.empty() ? "stream.binance.com" : test_hostname;
    std::string url = "wss://" + hostname + ":443/stream?streams=";
    for (std::size_t i = 0; i < canonical_symbols.size(); ++i) {
        url += canonical_symbols[i] + "@kline_1s";
        if (i + 1 < canonical_symbols.size()) url += "/";
    }
    if (!test_url.empty()) url = test_url;
    auto stream = std::make_shared<sentum::api::ReconnectingMarketStream>(url, hostname, stream_settings,
        [this](const std::string& payload) { handle_message(payload); });
    { std::lock_guard<std::mutex> stream_lock(impl->mutex); impl->stream = std::move(stream); }
    running.store(true, std::memory_order_release);
    producer_stopped.store(false, std::memory_order_release);
    queue_pressure_level_.store(static_cast<std::uint8_t>(sentum::market::QueuePressureLevel::Normal), std::memory_order_relaxed);
    auto& perf = sentum::market::RuntimePerformanceMetrics::global();
    perf.observe_queue_depth(queue.size_approx());
    perf.set_queue_pressure(sentum::market::QueuePressureLevel::Normal);
    try {
        logger.start();
        writer_thread = std::thread(&Collector::writer_loop, this);
        ws_thread = std::thread(&Collector::run, this);
        ws_worker_id = ws_thread.get_id();
    } catch (...) {
        running.store(false, std::memory_order_release);
        request_transport_stop();
        producer_stopped.store(true, std::memory_order_release);
        queue_cv.notify_all();
        join_workers(lock);
        logger.stop();
        throw;
    }
}

void Collector::request_transport_stop() {
    std::shared_ptr<sentum::api::ReconnectingMarketStream> stream;
    { std::lock_guard<std::mutex> lock(impl->mutex); stream = impl->stream; }
    if (stream) stream->stop();
}

void Collector::join_workers(std::unique_lock<std::mutex>& lock) {
    if (!ws_thread.joinable() && !writer_thread.joinable()) return;
    joining = true;
    lock.unlock();
    if (ws_thread.joinable()) ws_thread.join();
    producer_stopped.store(true, std::memory_order_release);
    queue_cv.notify_all();
    if (writer_thread.joinable()) writer_thread.join();
    lock.lock();
    ws_worker_id = {};
    joining = false;
    lifecycle_cv.notify_all();
}

void Collector::stop() {
    std::unique_lock<std::mutex> lock(lifecycle_mutex);
    running.store(false, std::memory_order_release);
    request_transport_stop();
    queue_cv.notify_all();
    if (ws_worker_id == std::this_thread::get_id()) return;
    lifecycle_cv.wait(lock, [this] { return !joining; });
    running.store(false, std::memory_order_release);
    request_transport_stop();
    join_workers(lock);
    update_queue_pressure(queue.size_approx(), false);
    logger.log("Collector stopped: enqueued=" + std::to_string(enqueued.load()) +
               " dropped=" + std::to_string(dropped.load()) +
               " drop_rate=" + std::to_string(drop_rate()) +
               " persistence=" + persistence_snapshot().dump());
    logger.stop();
}

bool Collector::try_enqueue(const std::string* symbol, Kline kline) {
    if (persistence_failed()) {
        persistence_rejected.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const auto result = queue.try_push_observed(KlineBatchItem{symbol, std::move(kline)});
    if (!result.accepted) {
        dropped.fetch_add(1, std::memory_order_relaxed);
        update_queue_pressure(queue_capacity, true);
        return false;
    }

    enqueued.fetch_add(1, std::memory_order_relaxed);
    update_queue_pressure(result.depth, false);
    if (result.was_empty) {
        sentum::market::RuntimePerformanceMetrics::global().observe_queue_wakeup();
        queue_cv.notify_one();
    }
    return true;
}

void Collector::writer_loop() {
    persistence_writer_active.store(true, std::memory_order_release);
    try {
        writer_loop_impl();
    } catch (...) {
        persistence_write_failures.fetch_add(1, std::memory_order_relaxed);
        fail_persistence(SQLITE_INTERNAL, SQLITE_OK, "Unexpected persistence writer failure");
    }
    persistence_writer_active.store(false, std::memory_order_release);
}

void Collector::writer_loop_impl() {
    using namespace std::chrono_literals;
    auto& batch = pending_batch;
    batch.reserve(batch_size);
    auto last_metrics = std::chrono::steady_clock::now();

    while (running.load(std::memory_order_acquire) ||
           !producer_stopped.load(std::memory_order_acquire) ||
           !queue.empty()) {
        KlineBatchItem item;
        while (batch.size() < batch_size && queue.try_pop(item)) batch.push_back(std::move(item));
        if (batch.empty()) {
            std::unique_lock<std::mutex> lock(wait_mutex);
            queue_cv.wait_for(lock, 100ms, [this] {
                return !queue.empty() ||
                       (!running.load(std::memory_order_acquire) && producer_stopped.load(std::memory_order_acquire));
            });
            continue;
        }

        update_queue_pressure(queue.size_approx(), false);
        const auto retry_deadline = std::chrono::steady_clock::now() + persistence_retry_budget;
        auto retry_delay = 25ms;
        for (std::size_t attempt = 0; ; ++attempt) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                retry_deadline - std::chrono::steady_clock::now());
            const int wait_ms = static_cast<int>(std::clamp<std::int64_t>(remaining.count(), 0, persistence_sqlite_wait_ms));
            KlineWriteResult result;
            {
                sentum::market::ScopedLatency latency(sentum::market::RuntimePerformanceMetrics::global().sqlite_batch_latency);
                result = db_ref.save_kline_batch_result(batch, wait_ms);
            }
            if (result.committed) {
                persisted.fetch_add(batch.size(), std::memory_order_release);
                persistence_state.store(PersistenceState::Healthy, std::memory_order_release);
                break;
            }

            persistence_write_failures.fetch_add(1, std::memory_order_relaxed);
            {
                std::lock_guard<std::mutex> lock(persistence_mutex);
                persistence_sqlite_code = result.sqlite_code;
                persistence_rollback_code = result.rollback_code;
            }
            const bool exhausted = attempt + 1 >= persistence_max_attempts ||
                                   std::chrono::steady_clock::now() >= retry_deadline;
            if (!result.retryable() || exhausted) {
                fail_persistence(result.sqlite_code, result.rollback_code,
                    result.rollback_code != SQLITE_OK ? "SQLite transaction rollback failed" :
                    result.retryable() ? "SQLite contention exceeded the retry budget" : "SQLite write rejected");
                return;
            }

            persistence_state.store(PersistenceState::Retrying, std::memory_order_release);
            persistence_retries.fetch_add(1, std::memory_order_relaxed);
            std::unique_lock<std::mutex> lock(wait_mutex);
            queue_cv.wait_until(lock, std::min(retry_deadline, std::chrono::steady_clock::now() + retry_delay), [this] {
                return !running.load(std::memory_order_acquire) && producer_stopped.load(std::memory_order_acquire);
            });
            retry_delay = std::min(retry_delay * 2, 200ms);
        }
        batch.clear();
        if (std::chrono::steady_clock::now() - last_metrics >= 10s) {
            const double rate = drop_rate();
            const auto perf_snapshot = sentum::market::RuntimePerformanceMetrics::global().snapshot();
            logger.log("Queue metrics: depth=" + std::to_string(queue.size_approx()) +
                       " high_water=" + std::to_string(perf_snapshot.value("queue_high_water", 0ULL)) +
                       " pressure=" + perf_snapshot.value("queue_pressure", std::string("unknown")) +
                       " wakeups=" + std::to_string(perf_snapshot.value("queue_wakeups", 0ULL)) +
                       " enqueued=" + std::to_string(enqueued.load()) +
                       " dropped=" + std::to_string(dropped.load()) +
                       " drop_rate=" + std::to_string(rate) +
                       " limit=" + std::to_string(max_drop_rate));
            if (rate > max_drop_rate) logger.log("WARNING: collector drop-rate limit exceeded");
            last_metrics = std::chrono::steady_clock::now();
        }
    }
}

void Collector::handle_message(const std::string& payload) {
    if (!running.load(std::memory_order_acquire)) return;
    sentum::collector::ParsedKline parsed;
    {
        sentum::market::SampledScopedLatency latency(
            sentum::market::RuntimePerformanceMetrics::global().parse_latency, parse_latency_sampler);
        if (!sentum::collector::FastBinanceKlineParser::parse(payload, parsed)) return;
    }
    const auto symbol = resolve_symbol(parsed.symbol);
    if (!symbol.canonical || parsed.timestamp <= 0 || parsed.timestamp > parsed.event_time || !std::isfinite(parsed.close) || parsed.close <= 0.0 ||
        !std::isfinite(parsed.open) || parsed.open <= 0.0 || !std::isfinite(parsed.high) || parsed.high <= 0.0 ||
        !std::isfinite(parsed.low) || parsed.low <= 0.0 || !std::isfinite(parsed.volume) || parsed.volume < 0.0) return;
    std::shared_ptr<sentum::api::ReconnectingMarketStream> stream;
    { std::lock_guard<std::mutex> lock(impl->mutex); stream = impl->stream; }
    if (!stream || !stream->accept_event(parsed.event_time)) return;
    const auto event_time = std::chrono::system_clock::time_point(std::chrono::milliseconds(parsed.event_time));
    Kline entry;
    entry.timestamp = parsed.timestamp;
    entry.open = parsed.open; entry.high = parsed.high; entry.low = parsed.low; entry.close = parsed.close; entry.volume = parsed.volume;
    store_ref.upsert(symbol.id, entry);
    auto& perf = sentum::market::RuntimePerformanceMetrics::global();
    perf.market_events.fetch_add(1, std::memory_order_relaxed);
    if (parsed.closed) {
        MarketEvent event;
        event.type = MarketEvent::Type::Candle;
        event.symbol_id = symbol.id;
        event.symbol = *symbol.canonical;
        event.timestamp = event_time;
        event.price = entry.close;
        event.open = entry.open; event.high = entry.high; event.low = entry.low; event.close = entry.close; event.volume = entry.volume; event.closed = true;
        {
            sentum::market::SampledScopedLatency latency(perf.event_dispatch_latency, dispatch_latency_sampler);
            sentum::market::MarketEventBus::global().publish(event);
        }
        try_enqueue(symbol.canonical, std::move(entry));
    }
}

void Collector::run() {
    try {
        std::shared_ptr<sentum::api::ReconnectingMarketStream> stream;
        { std::lock_guard<std::mutex> lock(impl->mutex); stream = impl->stream; }
        if (before_connect) before_connect();
        if (running.load(std::memory_order_acquire) && stream) stream->run();
    } catch (const std::exception& error) {
        if (running.load()) logger.log(std::string("Collector stream error: ") + error.what());
    }
    running.store(false, std::memory_order_release);
    producer_stopped.store(true, std::memory_order_release);
    queue_cv.notify_all();
    // A joined producer's thread id can be reused while its writer still
    // drains. Clear callback identity before this thread can be recycled.
    std::lock_guard<std::mutex> lock(lifecycle_mutex);
    ws_worker_id = {};
}
