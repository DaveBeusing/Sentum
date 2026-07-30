#pragma once

#include <string>
#include <vector>

#include <sentum/market/MarketDataStore.hpp>
#include <sentum/utils/Database.hpp>

struct SymbolPerformance {
    std::string symbol;
    double cum_return;
};

class SymbolScanner {
public:
    explicit SymbolScanner(Database& db, double threshold = 0.0005);
    explicit SymbolScanner(MarketDataStore& store, double threshold = 0.0005);
    std::vector<SymbolPerformance> fetch_top_performers(int lookback = 60, int max_symbols = 5);

private:
    MarketDataStore& store;
    double min_return_threshold;
};
