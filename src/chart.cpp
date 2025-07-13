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
#include "../include/chart.hpp"
#include <iostream>
#include <iomanip>
#include <algorithm>

void Chart::draw_price_chart(const std::vector<double>& prices, const std::string& symbol, const TradePosition& position) {
	if (prices.empty()) return;

	double min = *std::min_element(prices.begin(), prices.end());
	double max = *std::max_element(prices.begin(), prices.end());

	int height = 10;
	int width = prices.size();

	std::cout << "=== Chart ===\n";
	std::cout << std::fixed << std::setprecision(2);
	std::cout << "Current: $" << prices.back() << " | Min: $" << min << " | Max: $" << max << "\n\n";

	int sl_row = -1, tp_row = -1;
	if (position.open) {
		sl_row = (position.stop_loss_price - min) / (max - min + 0.0001) * height;
		tp_row = (position.take_profit_price - min) / (max - min + 0.0001) * height;
	}

	for (int row = height; row >= 0; --row) {
		for (size_t i = 0; i < prices.size(); ++i) {
			double scaled = (prices[i] - min) / (max - min + 0.0001);
			int level = scaled * height;

			if (row == sl_row && position.open) {
				std::cout << "\033[31m-\033[0m"; // SL = rot
			}
			else if (row == tp_row && position.open) {
				std::cout << "\033[32m=\033[0m"; // TP = grün
			}
			else if (level >= row) {
				std::string color = "\033[0m"; // default
				if (i > 0) {
					if (prices[i] > prices[i - 1]) color = "\033[32m"; // grün
					else if (prices[i] < prices[i - 1]) color = "\033[31m"; // rot
				}
				std::cout << color << "█" << "\033[0m";
			} else {
				std::cout << " ";
			}
		}
		std::cout << "\n";
	}

	std::cout << std::string(width, '-') << "\n";
	//std::cout << "↑ grün, ↓ rot — Ctrl+C zum Beenden\n";
	//std::cout << "█: Price | \033[31m-\033[0m: SL | \033[32m=\033[0m: TP | Ctrl+C to exit\n";
}


void Chart::draw_equity_chart(const std::vector<double>& equity_history, int win_count, int lose_count, double winrate, int total_trades, double average_profit) {
	if (equity_history.empty()) return;

	double min = *std::min_element(equity_history.begin(), equity_history.end());
	double max = *std::max_element(equity_history.begin(), equity_history.end());

	int height = 10;
	int width = equity_history.size();

	std::cout << "\n=== Equity-Monitoring ===\n";
	std::cout << "Total: " << equity_history.back() << " USDC | Min: " << min << " | Max: " << max << "\n";
	std::cout << "Trades: " << total_trades << " | Wins: " << win_count << " | Losses: " << lose_count << " | Winrate: " << winrate << "%\n";
	std::cout << "Avg Profit: " << average_profit << "\n\n";

	for (int row = height; row >= 0; --row) {
		for (size_t i = 0; i < equity_history.size(); ++i) {
			double scaled = (equity_history[i] - min) / (max - min + 0.0001);
			int level = scaled * height;
			if (level >= row) {
				std::string color = "\033[0m";
				if (i > 0 && equity_history[i] > equity_history[i - 1])
					color = "\033[32m"; // ↑ grün
				else if (i > 0 && equity_history[i] < equity_history[i - 1])
					color = "\033[31m"; // ↓ rot

				std::cout << color << "█" << "\033[0m";
			} else {
				std::cout << " ";
			}
		}
		std::cout << "\n";
	}

	std::cout << std::string(width, '-') << "\n";
	std::cout << "█: Equity | ↑ Profit | ↓ Loss\n";
}
