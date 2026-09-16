#pragma once

#include <functional>
#include <mutex>
#include <set>
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
    struct CachedReturns {
        std::string symbol;
        double return_30 = 0.0;
        double return_60 = 0.0;
        bool ready_30 = false;
        bool ready_60 = false;
    };

    struct RankedSymbol {
        double value = 0.0;
        sentum::market::SymbolId id = sentum::market::kInvalidSymbolId;
    };

    struct RankedSymbolCompare {
        bool operator()(const RankedSymbol& lhs, const RankedSymbol& rhs) const noexcept {
            if (lhs.value != rhs.value) return lhs.value > rhs.value;
            return lhs.id < rhs.id;
        }
    };

    void on_market_event(const MarketEvent& event);
    void update_id_cache(const MarketEvent& event, const MarketDataStore::ReturnSnapshot& returns);
    void update_legacy_cache(const MarketEvent& event);
    SymbolPerformance current_top_locked() const;

    MarketDataStore& store;
    double min_return_threshold;
    mutable std::mutex cache_mutex_;
    std::vector<CachedReturns> id_returns_;
    std::set<RankedSymbol, RankedSymbolCompare> ranking_30_;
    std::unordered_map<std::string, double> legacy_returns_30_;
    std::unordered_map<std::string, double> legacy_returns_60_;
    TopChangedHandler top_changed_handler_;
    std::string last_top_symbol_;
    sentum::market::MarketEventBus::SubscriptionId subscription_id_ = 0;
};
