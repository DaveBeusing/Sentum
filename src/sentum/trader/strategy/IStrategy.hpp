#pragma once

#include <algorithm>
#include <chrono>
#include <string>

#include <sentum/market/MarketEvent.hpp>
#include <sentum/trader/types/TradeAction.hpp>

struct StrategySignal {
    TradeAction action = TradeAction::NONE;
    std::string strategy;
    std::string reason;
    double reference_price = 0.0;
    std::chrono::system_clock::time_point created_at{};
    double confidence = 0.0;
};

class IStrategy {
public:
    virtual ~IStrategy() = default;
    virtual StrategySignal on_price(double price, std::chrono::system_clock::time_point observed_at) = 0;
    virtual StrategySignal on_event(const MarketEvent& event) {
        return on_price(event.price > 0.0 ? event.price : event.close, event.timestamp);
    }
    virtual void reset() = 0;
    virtual std::string name() const { return "strategy"; }
};
