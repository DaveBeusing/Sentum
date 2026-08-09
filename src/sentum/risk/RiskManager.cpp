/****
 * Copyright (C) 2025 Dave Beusing <david.beusing@gmail.com>
 * MIT License - https://opensource.org/license/mit/
 */

#include <chrono>

#include <sentum/risk/RiskManager.hpp>

using namespace std::chrono;

extern AsyncLogger logger;

namespace sentum::legacy {

RiskManager::RiskManager(double maxDaily, double maxTrade, int maxTrades)
    : max_daily_loss(maxDaily), max_trade_loss(maxTrade), max_trades_per_day(maxTrades),
      daily_loss(0), trades_today(0), last_reset(system_clock::now()) {}

bool RiskManager::allow_trade(double potential_loss) {
    if (potential_loss > max_trade_loss) {
        logger.log("[RISK] Trade rejected: exceeds max allowed trade loss");
        return false;
    }
    if (daily_loss + potential_loss > max_daily_loss) {
        logger.log("[RISK] Trade rejected: exceeds max allowed daily loss");
        return false;
    }
    if (trades_today >= max_trades_per_day) {
        logger.log("[RISK] Trade rejected: exceeds max trades per day");
        return false;
    }
    return true;
}

void RiskManager::record_trade(double pnl) {
    daily_loss += (pnl < 0 ? -pnl : 0);
    trades_today++;
}

void RiskManager::reset_day_if_needed() {
    const auto now = system_clock::now();
    const auto hours = duration_cast<std::chrono::hours>(now - last_reset).count();
    if (hours >= 24) {
        daily_loss = 0;
        trades_today = 0;
        last_reset = now;
        logger.log("[RISK] Daily counters reset.");
    }
}

double RiskManager::get_daily_loss() const { return daily_loss; }
int RiskManager::get_trades_today() const { return trades_today; }

} // namespace sentum::legacy
