#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>

#include <sentum/api/model/Kline.hpp>
#include <sentum/market/IncrementalIndicators.hpp>
#include <sentum/market/MarketDataStore.hpp>
#include <sentum/market/MarketEventBus.hpp>

int main() {
    constexpr std::size_t symbols = 500;
    constexpr std::size_t events_per_symbol = 2000;
    constexpr std::size_t total_events = symbols * events_per_symbol;

    MarketDataStore store(600);
    std::uint64_t delivered = 0;
    const auto subscription = sentum::market::MarketEventBus::global().subscribe(
        [&delivered](const MarketEvent&) { ++delivered; });

    sentum::market::RollingSma sma(20);
    sentum::market::Rsi rsi(14);
    sentum::market::RollingReturn rolling_return(60);

    const auto begin = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < total_events; ++i) {
        const std::string symbol = "sym" + std::to_string(i % symbols);
        const double price = 100.0 + static_cast<double>(i % 1000) * 0.001;
        Kline kline;
        kline.timestamp = static_cast<std::int64_t>(i) * 1000;
        kline.open = price - 0.01;
        kline.high = price + 0.02;
        kline.low = price - 0.02;
        kline.close = price;
        kline.volume = 1.0;
        store.upsert(symbol, kline);

        rolling_return.push(price);
        sma.push(price);
        rsi.push(price);

        MarketEvent event;
        event.type = MarketEvent::Type::Candle;
        event.symbol = symbol;
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
    store.cumulative_return("sym0", 60, return_60);

    std::cout << std::fixed << std::setprecision(2)
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
