#pragma once

#include <sentum/market/IncrementalIndicators.hpp>
#include <sentum/trader/strategy/IStrategy.hpp>

class MomentumStrategy final : public IStrategy {
public:
    explicit MomentumStrategy(std::size_t lookback = 20, double entry_threshold = 0.001)
        : lookback_(lookback), entry_threshold_(entry_threshold), rolling_return_(lookback) {}

    StrategySignal on_price(double price, std::chrono::system_clock::time_point observed_at) override {
        rolling_return_.push(price);
        if (!rolling_return_.ready()) return {};
        if (rolling_return_.value() >= entry_threshold_) {
            return {TradeAction::BUY, "momentum", "lookback return crossed entry threshold", price, observed_at};
        }
        return {};
    }

    void reset() override { rolling_return_.reset(); }

private:
    std::size_t lookback_;
    double entry_threshold_;
    sentum::market::RollingReturn rolling_return_;
};
