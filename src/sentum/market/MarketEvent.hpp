#pragma once

#include <chrono>
#include <string>

#include <sentum/market/SymbolId.hpp>

struct MarketEvent {
    enum class Type { Trade, Candle };
    Type type = Type::Trade;
    sentum::market::SymbolId symbol_id = sentum::market::kInvalidSymbolId;
    std::string symbol;
    std::chrono::system_clock::time_point timestamp{};
    double price = 0.0;
    double open = 0.0;
    double high = 0.0;
    double low = 0.0;
    double close = 0.0;
    double volume = 0.0;
    bool closed = true;
};
