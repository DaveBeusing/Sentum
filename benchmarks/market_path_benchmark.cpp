#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include <sentum/api/model/Kline.hpp>
#include <sentum/market/IncrementalIndicators.hpp>
#include <sentum/market/MarketDataStore.hpp>
#include <sentum/market/MarketEventBus.hpp>

int main(int argc, char** argv) {
    const std::size_t symbols = argc > 1 ? static_cast<std::size_t>(std::stoull(argv[1])) : 500;
    const std::size_t events_per_symbol = argc > 2 ? static_cast<std::size_t>(std::stoull(argv[2])) : 2000;
    const std::size_t total_events = symbols * events_per_symbol;
    if (symbols == 0 || events_per_symbol == 0 || symbols > 100000) return 2;

    MarketDataStore store(600);
    std::vector<std::string> names;
    names.reserve(symbols);
    for (std::size_t i = 0; i < symbols; ++i) {
        names.push_back("sym" + std::to_string(i));
        store.register_symbol(static_cast<sentum::market::SymbolId>(i + 1), names.back());
    }

    std::uint64_t delivered = 0;
    const auto subscription = sentum::market::MarketEventBus::global().subscribe(
        [&delivered](const MarketEvent&) { ++delivered; });

    sentum::market::RollingSma sma(20);
    sentum::market::Rsi rsi(14);
    sentum::market::RollingReturn rolling_return(60);

    const auto begin = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < total_events; ++i) {
        const auto index = i % symbols;
        const auto id = static_cast<sentum::market::SymbolId>(index + 1);
        const double price = 100.0 + static_cast<double>(i % 1000) * 0.001;
        Kline kline;
        kline.timestamp = static_cast<std::int64_t>(i) * 1000;
        kline.open = price - 0.01;
        kline.high = price + 0.02;
        kline.low = price - 0.02;
        kline.close = price;
        kline.volume = 1.0;
        store.upsert(id, kline);

        rolling_return.push(price);
        sma.push(price);
        rsi.push(price);

        MarketEvent event;
        event.type = MarketEvent::Type::Candle;
        event.symbol_id = id;
        event.symbol = names[index];
        event.price = price;
        event.close = price;
        event.closed = true;
        sentum::market::MarketEventBus::global().publish(event);
    }
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    sentum::market::MarketEventBus::global().unsubscribe(subscription);

    const double seconds = std::chrono::duration<double>(elapsed).count();
    const double eps = static_cast<double>(total_events) / seconds;
    double return_60 = 0.0;
    store.cumulative_return(static_cast<sentum::market::SymbolId>(1), 60, return_60);

    std::cout << std::fixed << std::setprecision(2)
              << "symbols=" << symbols << '\n'
              << "events=" << total_events << '\n'
              << "seconds=" << seconds << '\n'
              << "events_per_second=" << eps << '\n'
              << "nanoseconds_per_event=" << (seconds * 1e9 / static_cast<double>(total_events)) << '\n'
              << "delivered_events=" << delivered << '\n'
              << "rolling_return_60=" << return_60 << '\n'
              << "sma20=" << sma.current() << '\n'
              << "rsi14_ready=" << (rsi.ready() ? "true" : "false") << '\n';
    return delivered == total_events ? 0 : 1;
}
