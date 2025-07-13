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
#include "../include/config.hpp"
#include "../include/secrets.hpp"
#include "../include/binance.hpp"
#include "../include/rsi.hpp"
#include "../include/sma.hpp"
#include "../include/strategy.hpp"
#include "../include/trader.hpp"
#include "../include/chart.hpp"
#include "../include/style.hpp"

#include <iostream>
#include <vector>
#include <algorithm>
#include <iomanip>
#include <chrono>
#include <thread>
#include <mutex>
#include <ctime>

double last_price = 0.0;
const int MAX_POINTS = 60;
std::mutex mtx;

// Clear Terminal (ANSI)
void clear_terminal() {
	std::cout << "\033[2J\033[1;1H";
}

int main() {

	//Load secrets
	Secrets secrets = load_secrets("../config/secrets.json");
	if (secrets.api_key.empty() || secrets.api_secret.empty()) {
		std::cerr << "❌ Secrets missing! please check config/secrets.json.\n";
		return 1;
	}
	std::cout << "🔑 API-Key: ✅ (" << secrets.api_key.substr(0, 6) << "****)\n";

	//Initialize Binance
	Binance binance( secrets.api_key, secrets.api_secret );
	//test
	double usdc = binance.get_coin_balance("USDC");

	std::string symbol = "BTCUSDC";
	std::vector<double> price_history;
	std::vector<double> equity_history;

	//Load Risk config
	RiskConfig risk = load_risk_config("../config/risk.json");
	Trader trader(symbol, risk);

	//Start Stream
	start_ws_price_stream(symbol, [&](double price) {
		std::lock_guard<std::mutex> lock(mtx);

		// Update price history
		if (price_history.size() >= MAX_POINTS){
			price_history.erase(price_history.begin());
		}
		price_history.push_back(price);

		clear_terminal();

		// Current time
		std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
		std::time_t now_c = std::chrono::system_clock::to_time_t(now);
		//std::ostringstream oss;
		//oss << std::put_time(std::localtime(&now_c), "%d.%m.%Y %H:%M:%S");
		//std::string timestamp = oss.str();

		// Price changes
		std::string direction = "→";
		std::string color = "\033[0m";
		if (last_price > 0.0) {
			if (price > last_price) {
				direction = "↑"; color = "\033[32m";
			}
			else if (price < last_price) {
				direction = "↓"; color = "\033[31m";
			}
		}
		last_price = price;

		// Calculate RSI
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

		// Calculate SMAs
		double sma5 = calculate_sma(price_history, 5);
		double sma20 = calculate_sma(price_history, 20);
		// colored trend indicator
		std::string sma_info;
		if (sma5 > 0 && sma20 > 0) {
			std::string trend_color = "\033[0m";
			std::string trend_arrow = "→";
			if (sma5 > sma20) {
				trend_color = "\033[32m"; // green = upward trend
				trend_arrow = "↑";
			} else if (sma5 < sma20) {
				trend_color = "\033[31m"; // red = downward trend
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
				signal_str = "\033[32mSIGNAL: BUY \033[0m";
				break;
			case TradeSignal::SELL:
				signal_str = "\033[31mSIGNAL: SELL \033[0m";
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
				action_str = "\033[32mACTION: BUY \033[0m";
				break;
			case TradeAction::SELL:
				equity_history.push_back(trader.get_total_profit());
				action_str = "\033[31mACTION: SELL \033[0m";
				break;
			case TradeAction::NONE:
			default:
				action_str = "ACTION: HOLD";
				break;
		}

		// Top output
		std::cout << style::wrap("Sentum", style::bold()) << " - Intelligent Signals. Real-Time Decisions. Confident Trading.\n\n";
		//std::cout << style::wrap( timestamp, style::bold()) << "\n\n";
		std::cout << "\033[1m" << std::put_time(std::localtime(&now_c), "%d.%m.%Y %H:%M:%S") << "\033[0m\n\n";

		std::cout << "USDC Balance: " << usdc << "\n\n";

		std::cout << "Symbol: " << symbol << "\n";
		std::cout << color << direction << " Price: $" << price << " \033[0m \n"
			<< rsi_info << " | " << sma_info << " | " << signal_str << " | " << action_str << "\n\n";

		const TradePosition& pos = trader.get_position();
		if (pos.open) {
			double price = price_history.back();
			double sl_dist = price - pos.stop_loss_price;
			double tp_dist = pos.take_profit_price - price;
			double sl_pct = ((price - pos.stop_loss_price) / price) * 100.0;
			double tp_pct = ((pos.take_profit_price - price) / price) * 100.0;

			// calculate time in position
			std::chrono::seconds duration = std::chrono::duration_cast<std::chrono::seconds>(now - pos.entry_time);
			int minutes = static_cast<int>(duration.count() / 60);
			int seconds = static_cast<int>(duration.count() % 60);

			std::cout << " | In Position: " << std::setfill('0') << std::setw(2) << minutes << ":" << std::setfill('0') << std::setw(2) << seconds << "\n";
			std::cout << std::fixed << std::setprecision(2);
			std::cout << " | Stop-Loss: \033[31m$" << pos.stop_loss_price << " (" << sl_dist << " USDC, " << sl_pct << "% below current Price)\033[0m\n";
			std::cout << " | Take-Profit: \033[32m$" << pos.take_profit_price << " (" << tp_dist << " USDC, " << tp_pct << "% above current Price)\033[0m\n\n";
		}

		// update charts
		Chart::draw_price_chart(price_history, symbol, trader.get_position());
		Chart::draw_equity_chart(equity_history, trader.get_win_count(), trader.get_lose_count(), trader.get_winrate_percent(), trader.get_total_trades(), trader.get_average_profit() );

		// Bottom output
		std::cout << "Press Ctrl+C to exit\n";
	});

	std::cout << "📡 WebSocket started → Symbol: " << symbol << "\n";

	// Main thread just keeps running
	while (true) {
		std::this_thread::sleep_for(std::chrono::seconds(60));
	}

	return 0;
}