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
#include <numeric>

#include <sentum/strategy/IStrategy.hpp>

class SmaStrategy : public IStrategy {
	int short_period, long_period;
	double calculate_sma(const std::vector<double>& prices, int period) const {
		if (prices.size() < static_cast<size_t>(period)) return 0.0;
		double sum = std::accumulate(prices.end() - period, prices.end(), 0.0);
		return sum / static_cast<double>(period);
	}

	public:
		SmaStrategy(int short_p = 5, int long_p = 20) : short_period(short_p), long_period(long_p) {}
		std::string name() const override { return "SMA Crossover"; }
		Signal generate_signal(const std::vector<double>& prices) const override {
			if (prices.size() < static_cast<size_t>(long_period)) return Signal::HOLD;
			double sma_short = calculate_sma(prices, short_period);
			double sma_long = calculate_sma(prices, long_period);
			if (sma_short > sma_long) return Signal::BUY;
			if (sma_short < sma_long) return Signal::SELL;
			return Signal::HOLD;
		}
};