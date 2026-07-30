#include <algorithm>
#include <cmath>

#include <sentum/scanner/SymbolScanner.hpp>

constexpr double ROUND_FACTOR = 1e8;

SymbolScanner::SymbolScanner(MarketDataStore& store_, double threshold)
    : store(store_), min_return_threshold(threshold) {}

std::vector<SymbolPerformance> SymbolScanner::fetch_top_performers(int lookback, int max_symbols) {
    std::vector<SymbolPerformance> result;
    for (const auto& symbol : store.symbols()) {
        const auto klines = store.latest(symbol, static_cast<std::size_t>(lookback));
        if (klines.size() < 2 || klines.front().close <= 0.0) continue;
        const double cum_return = (klines.back().close - klines.front().close) / klines.front().close;
        if (cum_return > min_return_threshold) {
            result.push_back({symbol, std::round(cum_return * ROUND_FACTOR) / ROUND_FACTOR});
        }
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return a.cum_return > b.cum_return;
    });
    if (max_symbols > 0 && static_cast<int>(result.size()) > max_symbols) result.resize(max_symbols);
    return result;
}
