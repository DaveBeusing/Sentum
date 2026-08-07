#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <sentum/market/MarketDataStore.hpp>
#include <sentum/market/MarketEventBus.hpp>
#include <sentum/utils/Database.hpp>

struct SymbolPerformance {
    std::string symbol;
    double cum_return;
};

class SymbolScanner {
public:
    using TopChangedHandler = std::function<void(const SymbolPerformance&)>;

    explicit SymbolScanner(Database& db, double threshold = 0.0005);
    explicit SymbolScanner(MarketDataStore& store, double threshold = 0.0005);
    ~SymbolScanner();

    void set_top_changed_handler(TopChangedHandler handler);
    std::vector<SymbolPerformance> fetch_top_performers(int lookback = 60, int max_symbols = 5);

private:
    void on_market_event(const MarketEvent& event);
    void update_cache(const std::string& symbol, std::size_t lookback,
                      std::unordered_map<std::string, double>& cache);

    MarketDataStore& store;
    double min_return_threshold;
    mutable std::mutex cache_mutex_;
    std::unordered_map<std::string, double> returns_30_;
    std::unordered_map<std::string, double> returns_60_;
    TopChangedHandler top_changed_handler_;
    std::string last_top_symbol_;
    sentum::market::MarketEventBus::SubscriptionId subscription_id_ = 0;
};
