#pragma once

#include <algorithm>
#include <cmath>

#include <sentum/market/IncrementalIndicators.hpp>
#include <sentum/trader/strategy/IStrategy.hpp>

class MomentumStrategy final : public IStrategy {
public:
    explicit MomentumStrategy(std::size_t lookback = 20, double entry_threshold = 0.001)
        : lookback_(lookback), entry_threshold_(entry_threshold), rolling_return_(lookback) {}

    StrategySignal on_price(double price, std::chrono::system_clock::time_point observed_at) override {
        rolling_return_.push(price);
        if (!rolling_return_.ready()) return {};
        const double value = rolling_return_.value();
        if (value < entry_threshold_) return {};
        const double denominator = std::max(std::abs(entry_threshold_), 1e-9);
        const double confidence = std::clamp(value / denominator, 0.0, 2.0) / 2.0;
        return {TradeAction::BUY, name(), "lookback return crossed entry threshold", price, observed_at, confidence};
    }

    void reset() override { rolling_return_.reset(); }
    std::string name() const override { return "momentum"; }

private:
    std::size_t lookback_;
    double entry_threshold_;
    sentum::market::RollingReturn rolling_return_;
};
