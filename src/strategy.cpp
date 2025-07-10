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
#include "../include/strategy.hpp"
#include "../include/sma.hpp"
#include "../include/rsi.hpp"
#include <numeric>

Signal sma_crossover_strategy(const std::vector<double>& prices, int short_p, int long_p) {
	if (prices.size() < static_cast<std::size_t>(long_p)) return Signal::HOLD;
	double short_sma = calculate_sma(prices, short_p);
	double long_sma = calculate_sma(prices, long_p);
	if (short_sma > long_sma) return Signal::BUY;
	if (short_sma < long_sma) return Signal::SELL;
	return Signal::HOLD;
}

TradeSignal combined_sma_rsi_strategy(const std::vector<double>& prices, int sma_short, int sma_long, int rsi_period) {
	if (prices.size() < std::max(sma_long, rsi_period)) return TradeSignal::HOLD;
	double sma_s = calculate_sma(prices, sma_short);
	double sma_l = calculate_sma(prices, sma_long);
	double rsi = calculate_rsi(prices, rsi_period);
	if (sma_s > sma_l && rsi < 30) return TradeSignal::BUY;
	if (sma_s < sma_l && rsi > 70) return TradeSignal::SELL;
	return TradeSignal::HOLD;
}