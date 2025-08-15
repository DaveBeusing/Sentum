/****
 * Copyright (C) 2025 Dave Beusing <david.beusing@gmail.com>
 * 
 * MIT License - https://opensource.org/license/mit/
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the “Software”), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is furnished 
 * to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all 
 * copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A 
 * PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT 
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION 
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE 
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 * 
 */

#include <chrono>
#include <iostream>

#include <sentum/risk/RiskManager.hpp>

using namespace std::chrono;

extern AsyncLogger logger;

RiskManager::RiskManager(double maxDaily, double maxTrade, int maxTrades)
		: max_daily_loss(maxDaily),
		max_trade_loss(maxTrade),
		max_trades_per_day(maxTrades),
		daily_loss(0),
		trades_today(0),
		last_reset(system_clock::now()){}

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
	auto now = system_clock::now();
	auto hours = duration_cast<std::chrono::hours>(now - last_reset).count();
	if (hours >= 24) {
		daily_loss = 0;
		trades_today = 0;
		last_reset = now;
		logger.log("[RISK] Daily counters reset.");
	}
}

double RiskManager::get_daily_loss() const {
	return daily_loss;
}

int RiskManager::get_trades_today() const {
	return trades_today;
}