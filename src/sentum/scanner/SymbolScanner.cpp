#include <algorithm>
#include <cmath>

#include <sentum/scanner/SymbolScanner.hpp>

constexpr double ROUND_FACTOR = 1e8;

SymbolScanner::SymbolScanner(Database&, double threshold)
    : SymbolScanner(MarketDataStore::global(), threshold) {}

SymbolScanner::SymbolScanner(MarketDataStore& store_, double threshold)
    : store(store_), min_return_threshold(threshold) {
    subscription_id_ = sentum::market::MarketEventBus::global().subscribe(
        [this](const MarketEvent& event) { on_market_event(event); });
}

SymbolScanner::~SymbolScanner() {
    if (subscription_id_ != 0) sentum::market::MarketEventBus::global().unsubscribe(subscription_id_);
}

void SymbolScanner::update_cache(const std::string& symbol, std::size_t lookback,
                                 std::unordered_map<std::string, double>& cache) {
    double value = 0.0;
    if (!store.cumulative_return(symbol, lookback, value)) return;
    cache[symbol] = std::round(value * ROUND_FACTOR) / ROUND_FACTOR;
}

void SymbolScanner::on_market_event(const MarketEvent& event) {
    if (event.symbol.empty() || !event.closed) return;
    std::lock_guard<std::mutex> lock(cache_mutex_);
    update_cache(event.symbol, 30, returns_30_);
    update_cache(event.symbol, 60, returns_60_);
}

std::vector<SymbolPerformance> SymbolScanner::fetch_top_performers(int lookback, int max_symbols) {
    std::vector<SymbolPerformance> result;
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        const auto& cache = lookback <= 30 ? returns_30_ : returns_60_;
        result.reserve(cache.size());
        for (const auto& [symbol, cum_return] : cache) {
            if (cum_return > min_return_threshold) result.push_back({symbol, cum_return});
        }
    }
    const std::size_t wanted = max_symbols > 0 ? static_cast<std::size_t>(max_symbols) : result.size();
    if (wanted < result.size()) {
        std::partial_sort(result.begin(), result.begin() + static_cast<std::ptrdiff_t>(wanted), result.end(),
                          [](const auto& a, const auto& b) { return a.cum_return > b.cum_return; });
        result.resize(wanted);
    } else {
        std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
            return a.cum_return > b.cum_return;
        });
    }
    return result;
}
