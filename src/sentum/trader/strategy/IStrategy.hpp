#pragma once

#include <chrono>
#include <string>

#include <sentum/trader/types/TradeAction.hpp>

struct StrategySignal {
    TradeAction action = TradeAction::NONE;
    std::string strategy;
    std::string reason;
    double reference_price = 0.0;
    std::chrono::system_clock::time_point created_at{};
};

class IStrategy {
public:
    virtual ~IStrategy() = default;
    virtual StrategySignal on_price(double price, std::chrono::system_clock::time_point observed_at) = 0;
    virtual void reset() = 0;
};
