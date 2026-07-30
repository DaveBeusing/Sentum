#pragma once

#include <cmath>
#include <deque>

#include <sentum/trader/strategy/IStrategy.hpp>

class MomentumStrategy final : public IStrategy {
public:
    explicit MomentumStrategy(std::size_t lookback = 20, double entry_threshold = 0.001)
        : lookback_(lookback), entry_threshold_(entry_threshold) {}

    StrategySignal on_price(double price, std::chrono::system_clock::time_point observed_at) override {
        prices_.push_back(price);
        while (prices_.size() > lookback_) prices_.pop_front();
        if (prices_.size() < lookback_ || prices_.front() <= 0.0) return {};
        const double change = (price - prices_.front()) / prices_.front();
        if (change >= entry_threshold_) {
            return {TradeAction::BUY, "momentum", "lookback return crossed entry threshold", price, observed_at};
        }
        return {};
    }

    void reset() override { prices_.clear(); }

private:
    std::size_t lookback_;
    double entry_threshold_;
    std::deque<double> prices_;
};
