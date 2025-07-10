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
#include "../include/wsclient.hpp"
#include "../include/rsi.hpp"
#include "../include/sma.hpp"
#include "../include/strategy.hpp"
#include "../include/trader.hpp"

#include <iostream>
#include <vector>
#include <algorithm>
#include <iomanip>
#include <chrono>
#include <thread>
#include <mutex>

double last_price = 0.0;
const int MAX_POINTS = 60;
std::mutex mtx;

// Terminal löschen (ANSI)
void clear_terminal() {
	std::cout << "\033[2J\033[1;1H";
}

// Preis-Chart als ASCII
void draw_chart(const std::vector<double>& prices, const std::string& symbol) {
	if (prices.empty()) return;
	double min = *std::min_element(prices.begin(), prices.end());
	double max = *std::max_element(prices.begin(), prices.end());
	int height = 10;
	std::cout << "=== " << symbol << " Live Chart ===\n";
	std::cout << std::fixed << std::setprecision(2);
	std::cout << "Aktuell: $" << prices.back() << "   (Min: $" << min << " | Max: $" << max << ")\n\n";
	for (int row = height; row >= 0; --row) {
		for (size_t i = 0; i < prices.size(); ++i) {
			double scaled = (prices[i] - min) / (max - min + 0.0001);
			int level = scaled * height;
			if (level >= row) {
				std::string color = "\033[0m";  // default
				if (i > 0) {
					if (prices[i] > prices[i - 1]) color = "\033[32m";  // grün
					else if (prices[i] < prices[i - 1]) color = "\033[31m"; // rot
				}
				std::cout << color << "█" << "\033[0m";
			} else {
				std::cout << " ";
			}
		}
		std::cout << "\n";
	}
	std::cout << std::string(prices.size(), '-') << "\n";
	std::cout << "↑ grün, ↓ rot — Ctrl+C zum Beenden\n";
}

int main() {
	std::string symbol = "BTCUSDC";
	std::vector<double> price_history;
	RiskConfig risk = {1000.0, 0.1, 0.03, 0.05};  // Max 1k USD, 10%, SL: 3%, TP: 5%
	Trader trader(symbol, risk);

	start_ws_price_stream(symbol, [&](double price) {
		std::lock_guard<std::mutex> lock(mtx);

		// Preisverlauf aktualisieren
		if (price_history.size() >= MAX_POINTS)
			price_history.erase(price_history.begin());
		price_history.push_back(price);

		clear_terminal();

		// Preisänderung
		std::string direction = "→";
		std::string color = "\033[0m";
		if (last_price > 0.0) {
			if (price > last_price) { direction = "↑"; color = "\033[32m"; }
			else if (price < last_price) { direction = "↓"; color = "\033[31m"; }
		}
		last_price = price;

		// RSI berechnen
		double rsi = calculate_rsi(price_history);
		std::string rsi_info;
		if (rsi > 0) {
			int rsi_val = static_cast<int>(rsi);
		if (rsi_val > 70)
			rsi_info = "\033[31mRSI: " + std::to_string(rsi_val) + "\033[0m"; // rot – überkauft
		else if (rsi_val < 30)
			rsi_info = "\033[32mRSI: " + std::to_string(rsi_val) + "\033[0m"; // grün – überverkauft
		else
			rsi_info = "RSI: " + std::to_string(rsi_val); // neutral
		} else {
			rsi_info = "RSI: --";
		}

		// SMA-Werte berechnen
		double sma5 = calculate_sma(price_history, 5);
		double sma20 = calculate_sma(price_history, 20);
		// Farbige Trendanzeige
		std::string sma_info;
		if (sma5 > 0 && sma20 > 0) {
			std::string trend_color = "\033[0m";
			std::string trend_arrow = "→";
			if (sma5 > sma20) {
				trend_color = "\033[32m"; // grün = Aufwärtstrend
				trend_arrow = "↑";
			} else if (sma5 < sma20) {
				trend_color = "\033[31m"; // rot = Abwärtstrend
				trend_arrow = "↓";
			}
			sma_info = trend_color + "SMA5: " + std::to_string(static_cast<int>(sma5)) + " | SMA20: " + std::to_string(static_cast<int>(sma20)) + " " + trend_arrow + "\033[0m";
		} else {
			sma_info = "SMA: --";
		}

		TradeSignal signal = combined_sma_rsi_strategy(price_history, 5, 20, 14);
		std::string signal_str;
		switch (signal) {
			case TradeSignal::BUY:
				signal_str = "\033[32mSIGNAL: BUY 🚀\033[0m";
				break;
			case TradeSignal::SELL:
				signal_str = "\033[31mSIGNAL: SELL 🔻\033[0m";
				break;
			case TradeSignal::HOLD:
			default:
				signal_str = "SIGNAL: HOLD";
				break;
		}

		TradeAction action = trader.evaluate(price, sma5, sma20, rsi);
		std::string action_str;
		switch (action) {
			case TradeAction::BUY:
				action_str = "\033[32mEXECUTED: BUY ✅\033[0m";
				break;
			case TradeAction::SELL:
				action_str = "\033[31mEXECUTED: SELL ❌\033[0m";
				break;
			case TradeAction::NONE:
			default:
				action_str = "SIGNAL: HOLD";
				break;
		}

		// Ausgabe oben
		std::cout << color << direction << " Price: $" << price << " \033[0m | " << rsi_info << " | " << sma_info << " | " << signal_str << " | " << action_str << "\n\n";

		// Chart aktualisieren
		draw_chart(price_history, symbol);

		std::cout << "🏁 Total: " << trader.get_total_profit() << " USDC | 🟢 Wins: " << trader.get_win_count() << " | 🔴 Losses: " << trader.get_lose_count() << "\n";
	});

	std::cout << "📡 WebSocket gestartet für " << symbol << "\n";

	// Hauptthread läuft einfach weiter
	while (true) {
		std::this_thread::sleep_for(std::chrono::seconds(60));
	}

	return 0;
}