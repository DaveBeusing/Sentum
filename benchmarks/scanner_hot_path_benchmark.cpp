#include <sentum/market/MarketDataStore.hpp>
#include <sentum/market/MarketEventBus.hpp>
#include <sentum/scanner/SymbolScanner.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {
Kline make_kline(std::int64_t timestamp, double close) {
    Kline value;
    value.timestamp = timestamp;
    value.open = close;
    value.high = close;
    value.low = close;
    value.close = close;
    value.volume = 1.0;
    return value;
}
} // namespace

int main(int argc, char** argv) {
    const std::size_t symbols = argc > 1 ? static_cast<std::size_t>(std::stoull(argv[1])) : 1000;
    const std::size_t events_per_symbol = argc > 2 ? static_cast<std::size_t>(std::stoull(argv[2])) : 100;
    if (symbols == 0 || events_per_symbol == 0 || symbols > 100000) return 2;

    MarketDataStore store(128);
    std::vector<std::string> names;
    names.reserve(symbols);
    for (std::size_t index = 0; index < symbols; ++index) {
        names.push_back("sym" + std::to_string(index));
        const auto id = static_cast<sentum::market::SymbolId>(index + 1);
        store.register_symbol(id, names.back());
        for (std::int64_t sample = 1; sample <= 60; ++sample) {
            const double close = 100.0 + static_cast<double>(index % 17) * 0.01 + static_cast<double>(sample) * 0.001;
            store.upsert(id, make_kline(sample * 1000, close));
        }
    }

    SymbolScanner scanner(store, -1.0);
    std::uint64_t top_changes = 0;
    scanner.set_top_changed_handler([&top_changes](const SymbolPerformance&) { ++top_changes; });

    const std::size_t total_events = symbols * events_per_symbol;
    const auto begin = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < total_events; ++i) {
        const auto index = i % symbols;
        const auto round = i / symbols;
        const auto id = static_cast<sentum::market::SymbolId>(index + 1);
        const double close = 100.0 + static_cast<double>(index % 17) * 0.01 +
                             static_cast<double>(60 + round + 1) * 0.001;
        const auto timestamp = static_cast<std::int64_t>(61 + round) * 1000;
        store.upsert(id, make_kline(timestamp, close));

        MarketEvent event;
        event.type = MarketEvent::Type::Candle;
        event.symbol_id = id;
        event.symbol = names[index];
        event.price = close;
        event.close = close;
        event.closed = true;
        sentum::market::MarketEventBus::global().publish(event);
    }
    const auto elapsed = std::chrono::steady_clock::now() - begin;

    const auto top = scanner.fetch_top_performers(30, 5);
    const double seconds = std::chrono::duration<double>(elapsed).count();
    const double ns_per_event = seconds * 1e9 / static_cast<double>(total_events);
    const double eps = static_cast<double>(total_events) / seconds;

    std::cout << std::fixed << std::setprecision(2)
              << "symbols=" << symbols << '\n'
              << "events=" << total_events << '\n'
              << "seconds=" << seconds << '\n'
              << "events_per_second=" << eps << '\n'
              << "nanoseconds_per_event=" << ns_per_event << '\n'
              << "top_changes=" << top_changes << '\n'
              << "top_count=" << top.size() << '\n';

    return top.empty() ? 1 : 0;
}
