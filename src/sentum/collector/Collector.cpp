#include <chrono>
#include <mutex>
#include <stdexcept>

#include <boost/asio/ssl/context.hpp>
#include <nlohmann/json.hpp>
#include <websocketpp/client.hpp>
#include <websocketpp/config/asio_client.hpp>

#include <sentum/collector/Collector.hpp>
#include <sentum/utils/helper.hpp>

using json = nlohmann::json;
using client = websocketpp::client<websocketpp::config::asio_tls_client>;

struct Collector::Impl {
    client websocket;
    websocketpp::connection_hdl connection;
    std::mutex mutex;
    bool connection_valid = false;
};

Collector::Collector(Database& db, MarketDataStore& store, const std::vector<MarketInfo>& markets_)
    : db_ref(db), store_ref(store), markets(markets_), logger("log/collector.log"), impl(std::make_unique<Impl>()) {}

Collector::~Collector() { stop(); }

double Collector::drop_rate() const {
    const auto accepted = enqueued.load();
    const auto rejected = dropped.load();
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

bool Collector::try_enqueue(std::string symbol, Kline kline) {
    std::lock_guard<std::mutex> lock(queue_mutex);
    if (queue.size() >= queue_capacity) {
        dropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    queue.emplace_back(std::move(symbol), std::move(kline));
    enqueued.fetch_add(1, std::memory_order_relaxed);
    queue_cv.notify_one();
    return true;
}

void Collector::writer_loop() {
    using namespace std::chrono_literals;
    std::vector<std::pair<std::string, Kline>> batch;
    batch.reserve(batch_size);
    auto last_metrics = std::chrono::steady_clock::now();

    while (running.load() || !queue.empty()) {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            queue_cv.wait_for(lock, 100ms, [this] { return !queue.empty() || !running.load(); });
            while (!queue.empty() && batch.size() < batch_size) {
                batch.emplace_back(std::move(queue.front()));
                queue.pop_front();
            }
        }
        if (!batch.empty()) {
            if (!db_ref.save_kline_batch(batch)) logger.log("SQLite batch UPSERT failed, size=" + std::to_string(batch.size()));
            batch.clear();
        }
        if (std::chrono::steady_clock::now() - last_metrics >= 10s) {
            const double rate = drop_rate();
            logger.log("Queue metrics: depth=" + std::to_string(queue.size()) +
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
            std::lock_guard<std::mutex> lock(impl->mutex);
            impl->connection = hdl;
            impl->connection_valid = true;
        });
        impl->websocket.set_close_handler([this](websocketpp::connection_hdl) {
            std::lock_guard<std::mutex> lock(impl->mutex);
            impl->connection_valid = false;
        });
        impl->websocket.set_fail_handler([this](websocketpp::connection_hdl) {
            std::lock_guard<std::mutex> lock(impl->mutex);
            impl->connection_valid = false;
        });
        impl->websocket.set_message_handler([this](websocketpp::connection_hdl, client::message_ptr msg) {
            if (!running.load()) return;
            try {
                const auto payload = json::parse(msg->get_payload());
                if (!payload.contains("data") || !payload["data"].contains("k")) return;
                const auto& k = payload["data"]["k"];
                const std::string symbol = helper::to_lowercase(k["s"].get<std::string>());
                Kline entry;
                entry.timestamp = k["t"];
                entry.open = std::stod(k["o"].get<std::string>());
                entry.high = std::stod(k["h"].get<std::string>());
                entry.low = std::stod(k["l"].get<std::string>());
                entry.close = std::stod(k["c"].get<std::string>());
                entry.volume = std::stod(k["v"].get<std::string>());
                store_ref.upsert(symbol, entry);
                if (k.value("x", false)) try_enqueue(symbol, std::move(entry));
            } catch (const std::exception& e) {
                logger.log(std::string("parse error: ") + e.what());
            }
        });

        std::string url = "wss://stream.binance.com:443/stream?streams=";
        for (std::size_t i = 0; i < markets.size(); ++i) {
            url += helper::to_lowercase(markets[i].symbol) + "@kline_1s";
            if (i + 1 < markets.size()) url += "/";
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
