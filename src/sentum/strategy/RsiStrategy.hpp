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

#include <vector>
#include <string>

#include <sentum/strategy/IStrategy.hpp>

class RsiStrategy : public IStrategy {
	int period;
	double calculate_rsi(const std::vector<double>& prices) const {
		if (prices.size() <= period) return 50.0;
		double gain = 0, loss = 0;
		for (size_t i = 1; i <= period; ++i) {
			double change = prices[prices.size() - i] - prices[prices.size() - i - 1];
			if (change > 0) gain += change;
			else loss -= change;
		}
		if (gain + loss == 0) return 50.0;
		double rs = gain / loss;
		return 100.0 - (100.0 / (1.0 + rs));
	}

	public:
		RsiStrategy(int p = 14) : period(p) {}
		std::string name() const override { return "RSI"; }
		Signal generate_signal(const std::vector<double>& prices) const override {
			if (prices.size() < static_cast<size_t>(period + 1)) return Signal::HOLD;
			double rsi = calculate_rsi(prices);
			if (rsi < 30) return Signal::BUY;
			if (rsi > 70) return Signal::SELL;
			return Signal::HOLD;
		}
};