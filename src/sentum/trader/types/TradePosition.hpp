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

#pragma once

#include <chrono>
#include <string>

struct TradePosition {

	 // Status
	bool open = false;
	bool simulated = false;

	// Markets & Metadata
	std::string symbol;
	std::string source;
	std::string strategy;
	std::string notes;

	std::string order_type;
	std::string client_order_id;
	std::string exchange_order_id;

	// Entry
	double entry_price = 0.0;
	double quantity = 0.0;
	double executed_price = 0.0;
	int64_t execution_latency_ms = 0;
	std::chrono::system_clock::time_point signal_time;
	std::chrono::system_clock::time_point entry_time;

	// Dynamics during trade
	double highest_price = 0.0;
	double lowest_price = 0.0;
	double stop_loss_price = 0.0;
	double take_profit_price = 0.0;

	// Exit
	double exit_price = 0.0;
	std::chrono::system_clock::time_point exit_time;

	// Results
	double gross_profit = 0.0;
	double net_profit = 0.0;
	double fee_entry = 0.0;
	double fee_exit = 0.0;
	double initial_balance = 0.0;
	double closing_balance = 0.0;
	double capital_at_risk = 0.0;

	// Risk
	double leverage = 1.0;

	// Conditions
	std::string close_reason;
	bool stop_loss_triggered = false;
	bool take_profit_triggered = false;

	// Utility
	double holding_seconds() const {
		if (!open && entry_time != std::chrono::system_clock::time_point{}) {
			return std::chrono::duration<double>(exit_time - entry_time).count();
		}
		return 0.0;
	}

};