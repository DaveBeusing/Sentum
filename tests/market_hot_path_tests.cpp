#include <sentum/market/MarketDataStore.hpp>
#include <sentum/market/MarketEventBus.hpp>
#include <sentum/scanner/SymbolScanner.hpp>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

Kline candle(std::int64_t timestamp, double close) {
    Kline value;
    value.timestamp = timestamp;
    value.open = close;
    value.high = close;
    value.low = close;
    value.close = close;
    value.volume = 1.0;
    return value;
}

MarketEvent closed_event(sentum::market::SymbolId id, std::string symbol, double close) {
    MarketEvent event;
    event.type = MarketEvent::Type::Candle;
    event.symbol_id = id;
    event.symbol = std::move(symbol);
    event.close = close;
    event.price = close;
    event.closed = true;
    return event;
}

void seed(MarketDataStore& store, sentum::market::SymbolId id, double slope) {
    for (std::int64_t i = 1; i <= 60; ++i) {
        store.upsert(id, candle(i * 1000, 100.0 + static_cast<double>(i) * slope));
    }
}

void test_paired_return_snapshot_matches_single_reads() {
    MarketDataStore store(128);
    store.register_symbol(1, "BTCUSDT");
    seed(store, 1, 0.25);

    double expected_30 = 0.0;
    double expected_60 = 0.0;
    require(store.cumulative_return(1, 30, expected_30), "30-sample return was not ready");
    require(store.cumulative_return(1, 60, expected_60), "60-sample return was not ready");

    const auto snapshot = store.cumulative_returns(1, 30, 60);
    require(snapshot.first_ready, "paired 30-sample return was not ready");
    require(snapshot.second_ready, "paired 60-sample return was not ready");
    require(std::abs(snapshot.first - expected_30) < 1e-12, "paired 30-sample return diverged");
    require(std::abs(snapshot.second - expected_60) < 1e-12, "paired 60-sample return diverged");
}

void test_id_snapshot_remains_safe_during_registration() {
    MarketDataStore store(32);
    store.register_symbol(1, "SYM1");
    store.upsert(1, candle(1000, 100.0));

    std::atomic<bool> start{false};
    std::atomic<bool> failed{false};
    std::thread reader([&] {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        for (int i = 0; i < 20000; ++i) {
            if (store.size(1) != 1) failed.store(true, std::memory_order_relaxed);
        }
    });

    start.store(true, std::memory_order_release);
    for (sentum::market::SymbolId id = 2; id <= 256; ++id) {
        store.register_symbol(id, "SYM" + std::to_string(id));
        store.upsert(id, candle(1000, 100.0 + static_cast<double>(id)));
    }
    reader.join();

    require(!failed.load(std::memory_order_relaxed), "ID snapshot lookup changed during registration");
    require(store.size(256) == 1, "newly registered symbol was not visible through ID snapshot");
}

void test_scanner_incremental_top_ranking() {
    MarketDataStore store(128);
    store.register_symbol(1, "BTCUSDT");
    store.register_symbol(2, "ETHUSDT");
    seed(store, 1, 0.10);
    seed(store, 2, 0.20);

    std::vector<std::string> top_changes;
    {
        SymbolScanner scanner(store, 0.0);
        scanner.set_top_changed_handler([&](const SymbolPerformance& top) {
            top_changes.push_back(top.symbol);
        });

        sentum::market::MarketEventBus::global().publish(closed_event(1, "BTCUSDT", 106.0));
        sentum::market::MarketEventBus::global().publish(closed_event(2, "ETHUSDT", 112.0));

        store.upsert(1, candle(61000, 150.0));
        sentum::market::MarketEventBus::global().publish(closed_event(1, "BTCUSDT", 150.0));

        const auto top_30 = scanner.fetch_top_performers(30, 2);
        require(top_30.size() == 2, "scanner did not expose both ranked symbols");
        require(top_30.front().symbol == "BTCUSDT", "scanner incremental ranking did not promote BTCUSDT");
        require(top_30.front().cum_return >= top_30.back().cum_return, "scanner ranking order is not descending");

        const auto top_60 = scanner.fetch_top_performers(60, 2);
        require(top_60.size() == 2, "scanner 60-sample cache did not retain both symbols");
        require(top_60.front().symbol == "BTCUSDT", "scanner 60-sample ranking did not reflect updated store data");
    }

    require(top_changes.size() >= 3, "scanner did not report expected top-symbol transitions");
    require(top_changes[0] == "BTCUSDT", "first scanner top transition was unexpected");
    require(top_changes[1] == "ETHUSDT", "second scanner top transition was unexpected");
    require(top_changes.back() == "BTCUSDT", "scanner did not report BTCUSDT returning to top");
}
} // namespace

int main() {
    try {
        test_paired_return_snapshot_matches_single_reads();
        test_id_snapshot_remains_safe_during_registration();
        test_scanner_incremental_top_ranking();
        std::cout << "market hot path tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "market hot path test failure: " << error.what() << '\n';
        return 1;
    }
}
