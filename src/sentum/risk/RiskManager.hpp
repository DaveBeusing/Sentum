/****
 * Copyright (C) 2025 Dave Beusing <david.beusing@gmail.com>
 * MIT License - https://opensource.org/license/mit/
 */
#pragma once

#include <chrono>

#include <sentum/utils/AsyncLogger.hpp>

namespace sentum::legacy {

class RiskManager {
public:
    RiskManager(double maxDailyLoss, double maxTradeLoss, int maxTradesPerDay);
    bool allow_trade(double potential_loss);
    void record_trade(double pnl);
    void reset_day_if_needed();
    double get_daily_loss() const;
    int get_trades_today() const;

private:
    double max_daily_loss;
    double max_trade_loss;
    int max_trades_per_day;
    double daily_loss;
    int trades_today;
    std::chrono::system_clock::time_point last_reset;
};

} // namespace sentum::legacy
