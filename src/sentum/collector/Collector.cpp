#include <chrono>
#include <mutex>
#include <stdexcept>

#include <boost/asio/ssl/context.hpp>
#include <websocketpp/client.hpp>
#include <websocketpp/config/asio_client.hpp>

#include <sentum/collector/Collector.hpp>
#include <sentum/collector/FastBinanceKlineParser.hpp>
#include <sentum/market/MarketEventBus.hpp>
#include <sentum/market/RuntimePerformanceMetrics.hpp>
#include <sentum/utils/helper.hpp>

using client = websocketpp::client<websocketpp::config::asio_tls_client>;

struct Collector::Impl {
    client websocket;
    websocketpp::connection_hdl connection;
    std::mutex mutex;
    bool connection_valid = false;
};

Collector::Collector(Database& db, const std::vector<MarketInfo>& markets_)
    : Collector(db, MarketDataStore::global(), markets_) {}

Collector::Collector(Database& db, MarketDataStore& store, const std::vector<MarketInfo>& markets_)
    : db_ref(db), store_ref(store), markets(markets_), logger("log/collector.log"), impl(std::make_unique<Impl>()) {
    initialize_symbols();
}

Collector::~Collector() { stop(); }

void Collector::initialize_symbols() {
    canonical_symbols.reserve(markets.size());
    symbol_by_hash.reserve(markets.size() * 2);
    for (std::size_t i = 0; i < markets.size(); ++i) {
        canonical_symbols.push_back(helper::to_lowercase(markets[i].symbol));
        symbol_by_hash.emplace(sentum::market::symbol_hash(markets[i].symbol), i);
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

void Collector::start() {
    if (running.exchange(true)) return;
    logger.start();
    writer_thread = std::thread(&Collector::writer_loop, this);
    ws_thread = std::thread(&Collector::run, this);
}

void Collector::stop() {
    running.store(false);
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        if (impl->connection_valid) {
            websocketpp::lib::error_code ec;
            impl->websocket.close(impl->connection, websocketpp::close::status::going_away, "shutdown", ec);
        }
    }
    impl->websocket.stop_perpetual();
    impl->websocket.stop();
    queue_cv.notify_all();
    if (ws_thread.joinable() && ws_thread.get_id() != std::this_thread::get_id()) ws_thread.join();
    if (writer_thread.joinable() && writer_thread.get_id() != std::this_thread::get_id()) writer_thread.join();
    logger.log("Collector stopped: enqueued=" + std::to_string(enqueued.load()) +
               " dropped=" + std::to_string(dropped.load()) +
               " drop_rate=" + std::to_string(drop_rate()));
    logger.stop();
}

bool Collector::try_enqueue(const std::string* symbol, Kline kline) {
    if (!queue.try_push(KlineBatchItem{symbol, std::move(kline)})) {
        dropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    enqueued.fetch_add(1, std::memory_order_relaxed);
    const auto depth = queue.size_approx();
    sentum::market::RuntimePerformanceMetrics::global().observe_queue_depth(depth);
    queue_cv.notify_one();
    return true;
}

void Collector::writer_loop() {
    using namespace std::chrono_literals;
    std::vector<KlineBatchItem> batch;
    batch.reserve(batch_size);
    auto last_metrics = std::chrono::steady_clock::now();

    while (running.load(std::memory_order_acquire) || !queue.empty()) {
        KlineBatchItem item;
        while (batch.size() < batch_size && queue.try_pop(item)) batch.push_back(std::move(item));
        if (batch.empty()) {
            std::unique_lock<std::mutex> lock(wait_mutex);
            queue_cv.wait_for(lock, 100ms, [this] { return !queue.empty() || !running.load(); });
            continue;
        }
        {
            sentum::market::ScopedLatency latency(sentum::market::RuntimePerformanceMetrics::global().sqlite_batch_latency);
            if (!db_ref.save_kline_batch(batch)) logger.log("SQLite batch UPSERT failed, size=" + std::to_string(batch.size()));
        }
        batch.clear();
        if (std::chrono::steady_clock::now() - last_metrics >= 10s) {
            const double rate = drop_rate();
            logger.log("Queue metrics: depth=" + std::to_string(queue.size_approx()) +
                       " high_water=" + std::to_string(sentum::market::RuntimePerformanceMetrics::global().queue_high_water.load()) +
                       " enqueued=" + std::to_string(enqueued.load()) +
                       " dropped=" + std::to_string(dropped.load()) +
                       " drop_rate=" + std::to_string(rate) +
                       " limit=" + std::to_string(max_drop_rate));
            if (rate > max_drop_rate) logger.log("WARNING: collector drop-rate limit exceeded");
            last_metrics = std::chrono::steady_clock::now();
        }
    }
}

void Collector::run() {
    try {
        impl->websocket.init_asio();
        impl->websocket.start_perpetual();
        impl->websocket.clear_access_channels(websocketpp::log::alevel::all);
        impl->websocket.clear_error_channels(websocketpp::log::elevel::all);
        impl->websocket.set_tls_init_handler([](websocketpp::connection_hdl) {
            auto ctx = websocketpp::lib::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::tls_client);
            ctx->set_options(boost::asio::ssl::context::default_workarounds | boost::asio::ssl::context::no_sslv2 | boost::asio::ssl::context::no_sslv3);
            return ctx;
        });
        impl->websocket.set_open_handler([this](websocketpp::connection_hdl hdl) {
            std::lock_guard<std::mutex> lock(impl->mutex); impl->connection = hdl; impl->connection_valid = true;
        });
        impl->websocket.set_close_handler([this](websocketpp::connection_hdl) {
            std::lock_guard<std::mutex> lock(impl->mutex); impl->connection_valid = false;
        });
        impl->websocket.set_fail_handler([this](websocketpp::connection_hdl) {
            std::lock_guard<std::mutex> lock(impl->mutex); impl->connection_valid = false;
        });
        impl->websocket.set_message_handler([this](websocketpp::connection_hdl, client::message_ptr msg) {
            if (!running.load(std::memory_order_relaxed)) return;
            sentum::collector::ParsedKline parsed;
            {
                sentum::market::ScopedLatency latency(sentum::market::RuntimePerformanceMetrics::global().parse_latency);
                if (!sentum::collector::FastBinanceKlineParser::parse(msg->get_payload(), parsed)) return;
            }
            const auto symbol = resolve_symbol(parsed.symbol);
            if (!symbol.canonical) return;
            Kline entry;
            entry.timestamp = parsed.timestamp;
            entry.open = parsed.open; entry.high = parsed.high; entry.low = parsed.low; entry.close = parsed.close; entry.volume = parsed.volume;
            store_ref.upsert(*symbol.canonical, entry);
            auto& perf = sentum::market::RuntimePerformanceMetrics::global();
            perf.market_events.fetch_add(1, std::memory_order_relaxed);

            if (parsed.closed) {
                MarketEvent event;
                event.type = MarketEvent::Type::Candle;
                event.symbol_id = symbol.id;
                event.symbol = *symbol.canonical;
                event.timestamp = std::chrono::system_clock::time_point(std::chrono::milliseconds(entry.timestamp));
                event.price = entry.close;
                event.open = entry.open; event.high = entry.high; event.low = entry.low; event.close = entry.close; event.volume = entry.volume; event.closed = true;
                {
                    sentum::market::ScopedLatency latency(perf.event_dispatch_latency);
                    sentum::market::MarketEventBus::global().publish(event);
                }
                try_enqueue(symbol.canonical, std::move(entry));
            }
        });

        std::string url = "wss://stream.binance.com:443/stream?streams=";
        for (std::size_t i = 0; i < canonical_symbols.size(); ++i) {
            url += canonical_symbols[i] + "@kline_1s";
            if (i + 1 < canonical_symbols.size()) url += "/";
        }
        websocketpp::lib::error_code ec;
        auto con = impl->websocket.get_connection(url, ec);
        if (ec) throw std::runtime_error("Connection error: " + ec.message());
        impl->websocket.connect(con);
        impl->websocket.run();
    } catch (const std::exception& e) {
        if (running.load()) logger.log(std::string("Collector run() error: ") + e.what());
    }
    running.store(false);
    queue_cv.notify_all();
}
