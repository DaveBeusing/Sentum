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
#include <string>

struct RiskConfig {
	double max_total_usdt = 1000.0;			// z. B. Gesamtbudget
	double risk_per_trade = 0.1;			// z. B. 10 % pro Trade
	double stop_loss_percent = 0.03;		// z. B. 3 % SL
	double take_profit_percent = 0.05;		// z. B. 5 % TP (optional)
	double fee_percent = 0.001;				// 0.1% Standard Binance Fee (0.095% Taker 0.1% Maker)
};

struct TradePosition {
	bool open = false;
	double entry_price = 0.0;
	double quantity = 0.0;
	double highest_price = 0.0;
	double stop_loss_price = 0.0;
	double take_profit_price = 0.0;
};

enum class TradeAction { NONE, BUY, SELL };

class Trader {

	public:
		Trader(std::string symbol, RiskConfig config);
		TradeAction evaluate(double price, double sma5, double sma20, double rsi);
		void log_trade(TradeAction action, double price);
		const TradePosition& get_position() const;
		// Performance
		double get_total_profit() const;
		int get_win_count() const;
		int get_lose_count() const;

	private:
		std::string symbol;
		RiskConfig risk;
		TradePosition position;
		// Performance
		double total_profit;
		int win_count;
		int lose_count;
};

