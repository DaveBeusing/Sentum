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

#include <iostream>
#include <fstream>
#include <chrono>
#include <iomanip>
#include <thread>

#include <sentum/trader/trader.hpp>


Trader::Trader(const std::string& symbol_, Binance& api_) : symbol(symbol_), api(api_), running(true) {}

void Trader::stop() {
	running = false;
	if (price_stream) price_stream->stop();
}

void Trader::run() {
	using namespace std::chrono_literals;

	price_stream = std::make_unique<Websocket>(symbol);
	price_stream->set_on_price([this](double price) {
		this->latest_price.store(price);
		// double sma5 = ..., sma20 = ..., rsi = ...;
		// evaluate(price, sma5, sma20, rsi);
	});
	price_stream->start();
	while (running) {
		std::this_thread::sleep_for(500ms);
	}
	//std::cout << "Trader stopped → " << symbol << "\n";
}



//Trader::Trader(std::string symbol, RiskConfig config) : symbol(std::move(symbol)), risk(config), total_profit(0.0), win_count(0), lose_count(0) {}

TradeAction Trader::evaluate(double price, double sma5, double sma20, double rsi) {
	if (!position.open) {
		if (sma5 > sma20 && rsi < 30) {
			// BUY
			position.open = true;
			position.entry_price = price;
			position.quantity = (risk.max_total_usdt * risk.risk_per_trade) / price;
			position.highest_price = price;
			position.stop_loss_price = price * (1.0 - risk.stop_loss_percent);
			position.take_profit_price = price * (1.0 + risk.take_profit_percent);
			position.entry_time = std::chrono::system_clock::now();
			log_trade(TradeAction::BUY, price);
			return TradeAction::BUY;
		}
	} else {
		// Trailing Stop-Loss nachziehen
		if (price > position.highest_price) {
			position.highest_price = price;
			position.stop_loss_price = price * (1.0 - risk.stop_loss_percent);
		}
		// Bedingungen für Exit
		bool stop_loss_hit = price <= position.stop_loss_price;
		bool take_profit_hit = price >= position.take_profit_price;
		bool sell_signal = (sma5 < sma20 && rsi > 70);
		//if (stop_loss_hit || take_profit_hit || sell_signal) {
		if (stop_loss_hit || take_profit_hit) {
			log_trade(TradeAction::SELL, price);
			// Profit/Verlust berechnen
			double gross_profit = (price - position.entry_price) * position.quantity;
			double buy_fee = position.entry_price * position.quantity * risk.fee_percent;
			double sell_fee = price * position.quantity * risk.fee_percent;
			double net_profit = gross_profit - buy_fee - sell_fee;
			total_profit += net_profit;
			if (net_profit >= 0.0) {
				win_count++;
			} else {
				lose_count++;
			}

			std::cout << "[RESULT] Trade Result: "
						<< (net_profit >= 0 ? "\033[32m" : "\033[31m")
						<< (net_profit >= 0 ? "+" : "") << net_profit << " USDT"
						<< "\033[0m | Total: " << total_profit << " USDT"
						<< " | Wins: " << win_count << " | Losses: " << lose_count << "\n";

			position = TradePosition(); // Reset
			return TradeAction::SELL;
		}
	}
	return TradeAction::NONE;
}

void Trader::log_trade(TradeAction action, double price) {
	std::ofstream file("trades.csv", std::ios::app);
	auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
	std::stringstream timestamp;
	timestamp << std::put_time(std::localtime(&now), "%Y-%m-%d %H:%M:%S");
	std::string action_str = (action == TradeAction::BUY ? "BUY" : "SELL");
	file << timestamp.str() << "," << action_str << "," << symbol << "," << price << "," << position.quantity;
	if (action == TradeAction::SELL) {
		double gross_profit = (price - position.entry_price) * position.quantity;
		double buy_fee = position.entry_price * position.quantity * risk.fee_percent;
		double sell_fee = price * position.quantity * risk.fee_percent;
		double net_profit = gross_profit - buy_fee - sell_fee;
		file << "," << net_profit;
	}
	file << "\n";
	file.close();
	std::cout << "[TRADE] " << action_str << " " << position.quantity << " " << symbol << " @ $" << price << std::endl;
}

const TradePosition& Trader::get_position() const {
	return position;
}

TradePosition Trader::get_current_position() const {
	return position;
}

double Trader::get_latest_price() const {
	return latest_price.load();
}

double Trader::get_total_profit() const {
	return total_profit;
}

int Trader::get_win_count() const {
	return win_count;
}

int Trader::get_lose_count() const {
	return lose_count;
}

double Trader::get_winrate_percent() const {
	int total = win_count + lose_count;
	if (total == 0) return 0.0;
	return (static_cast<double>(win_count) / total) * 100.0;
}

int Trader::get_total_trades() const {
	return win_count + lose_count;
}

double Trader::get_average_profit() const {
	int total = get_total_trades();
	if (total == 0) return 0.0;
	return total_profit / static_cast<double>(total);
}