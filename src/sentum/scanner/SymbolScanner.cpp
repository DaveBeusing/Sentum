#include <algorithm>
#include <cmath>

#include <sentum/scanner/SymbolScanner.hpp>

namespace {
constexpr double ROUND_FACTOR = 1e8;

double rounded_return(double value) noexcept {
    return std::round(value * ROUND_FACTOR) / ROUND_FACTOR;
}
} // namespace

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

void SymbolScanner::set_top_changed_handler(TopChangedHandler handler) {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    top_changed_handler_ = std::move(handler);
}

void SymbolScanner::update_id_cache(const MarketEvent& event, const MarketDataStore::ReturnSnapshot& returns) {
    const auto index = static_cast<std::size_t>(event.symbol_id);
    if (id_returns_.size() <= index) id_returns_.resize(index + 1);
    auto& cached = id_returns_[index];
    if (cached.symbol != event.symbol) cached.symbol = event.symbol;

    if (returns.first_ready) {
        if (cached.ready_30 && cached.return_30 > min_return_threshold) {
            ranking_30_.erase(RankedSymbol{cached.return_30, event.symbol_id});
        }
        cached.return_30 = rounded_return(returns.first);
        cached.ready_30 = true;
        if (cached.return_30 > min_return_threshold) {
            ranking_30_.insert(RankedSymbol{cached.return_30, event.symbol_id});
        }
    }

    if (returns.second_ready) {
        cached.return_60 = rounded_return(returns.second);
        cached.ready_60 = true;
    }
}

void SymbolScanner::update_legacy_cache(const MarketEvent& event) {
    double value = 0.0;
    if (store.cumulative_return(event.symbol, 30, value)) {
        legacy_returns_30_[event.symbol] = rounded_return(value);
    }
    if (store.cumulative_return(event.symbol, 60, value)) {
        legacy_returns_60_[event.symbol] = rounded_return(value);
    }
}

SymbolPerformance SymbolScanner::current_top_locked() const {
    SymbolPerformance top;
    if (!ranking_30_.empty()) {
        const auto& ranked = *ranking_30_.begin();
        const auto index = static_cast<std::size_t>(ranked.id);
        if (index < id_returns_.size()) {
            top = {id_returns_[index].symbol, ranked.value};
        }
    }

    for (const auto& [symbol, value] : legacy_returns_30_) {
        if (value <= min_return_threshold) continue;
        if (top.symbol.empty() || value > top.cum_return) top = {symbol, value};
    }
    return top;
}

void SymbolScanner::on_market_event(const MarketEvent& event) {
    if (event.symbol.empty() || !event.closed) return;

    MarketDataStore::ReturnSnapshot returns;
    const bool has_id = event.symbol_id != sentum::market::kInvalidSymbolId;
    if (has_id) returns = store.cumulative_returns(event.symbol_id, 30, 60);

    TopChangedHandler handler;
    SymbolPerformance top;
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        if (has_id) {
            update_id_cache(event, returns);
        } else {
            update_legacy_cache(event);
        }

        top = current_top_locked();
        if (!top.symbol.empty() && top.symbol != last_top_symbol_) {
            last_top_symbol_ = top.symbol;
            handler = top_changed_handler_;
            changed = true;
        }
    }
    if (changed && handler) handler(top);
}

std::vector<SymbolPerformance> SymbolScanner::fetch_top_performers(int lookback, int max_symbols) {
    std::vector<SymbolPerformance> result;
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        result.reserve(id_returns_.size() + legacy_returns_60_.size());

        const bool use_30 = lookback <= 30;
        for (std::size_t index = 1; index < id_returns_.size(); ++index) {
            const auto& cached = id_returns_[index];
            const bool ready = use_30 ? cached.ready_30 : cached.ready_60;
            const double value = use_30 ? cached.return_30 : cached.return_60;
            if (ready && value > min_return_threshold && !cached.symbol.empty()) {
                result.push_back({cached.symbol, value});
            }
        }

        const auto& legacy = use_30 ? legacy_returns_30_ : legacy_returns_60_;
        for (const auto& [symbol, value] : legacy) {
            if (value > min_return_threshold) result.push_back({symbol, value});
        }
    }

    const std::size_t wanted = max_symbols > 0 ? static_cast<std::size_t>(max_symbols) : result.size();
    if (wanted < result.size()) {
        std::partial_sort(result.begin(), result.begin() + static_cast<std::ptrdiff_t>(wanted), result.end(),
                          [](const auto& lhs, const auto& rhs) { return lhs.cum_return > rhs.cum_return; });
        result.resize(wanted);
    } else {
        std::sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.cum_return > rhs.cum_return;
        });
    }
    return result;
}
