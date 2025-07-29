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
#include <future>

#include <sentum/trader/TradeEngine.hpp>


TradeEngine::TradeEngine(const std::string& symbol_, Binance& api_) : symbol(symbol_), api(api_), running(true), engine_logger("log/engine.log") {}

void TradeEngine::stop() {
	running = false;
	if (price_stream) price_stream->stop();
	engine_logger.stop();
}

void TradeEngine::run() {
	engine_logger.start();
	risk = load_risk_config("config/risk.json");
	price_stream = std::make_unique<Websocket>(symbol);
	price_stream->set_on_price([this](double price) {
		this->latest_price.store(price);
		std::thread([this, price] {
			auto start = std::chrono::high_resolution_clock::now();
			TradeAction action = evaluate(price);
			auto end = std::chrono::high_resolution_clock::now();
			auto latency = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
			if (latency > 500) {
				engine_logger.log("High latency: " + std::to_string(latency) + " µs");
			}
		}).detach();
	});
	price_stream->start();
	while (running) {
		std::this_thread::sleep_for(std::chrono::milliseconds(50));//kürzerer intervall less blocking
	}
}

TradeAction TradeEngine::evaluate(double price) {

	if (!position.open) {

		// BUY
		position.open = true;
		position.symbol = symbol;
		position.entry_price = price;
		position.executed_price = price; //TODO: update with real Data from Order
		position.entry_time = std::chrono::system_clock::now();
		position.signal_time = std::chrono::system_clock::now();
		position.quantity = (risk.max_total_capital * risk.risk_per_trade) / price;
		position.highest_price = price;
		position.lowest_price = price;
		position.stop_loss_price = price * (1.0 - risk.stop_loss_percent);
		position.take_profit_price = price * (1.0 + risk.take_profit_percent);
		// Risikokonfiguration
		position.risk_per_trade = risk.risk_per_trade;
		position.stop_loss_percent = risk.stop_loss_percent;
		position.take_profit_percent = risk.take_profit_percent;
		position.trailing_sl_enabled = risk.trailing_sl_enabled;
		position.trailing_sl_percent = risk.trailing_sl_percent;
		position.trailing_tp_enabled = risk.trailing_tp_enabled;
		position.trailing_tp_percent = risk.trailing_tp_percent;
		// Gebührenkonfiguration
		position.buy_fee_percent = risk.buy_fee_percent;
		position.sell_fee_percent = risk.sell_fee_percent;
		logger.log(position, TradeAction::BUY);
		return TradeAction::BUY;

	} else {

		// Update Trailing Stop-Loss & TP
		if (price > position.highest_price) {
			position.highest_price = price;
			if (position.trailing_sl_enabled){
				position.stop_loss_price = price * (1.0 - position.trailing_sl_percent);
			}
			if (position.trailing_tp_enabled){
				position.take_profit_price = price * (1.0 - position.trailing_tp_percent);
			}
		}

		// set lowest price seen
		if (price < position.lowest_price) {
			position.lowest_price = price;
		}

		// SELL
		if (price <= position.stop_loss_price || price >= position.take_profit_price) {
			position.exit_price = price;
			position.exit_time = std::chrono::system_clock::now();
			position.stop_loss_triggered = (price <= position.stop_loss_price);
			position.take_profit_triggered = (price >= position.take_profit_price);
			position.gross_profit = (price - position.entry_price) * position.quantity;
			position.fee_entry = position.entry_price * position.quantity * position.buy_fee_percent;
			position.fee_exit = price * position.quantity * position.sell_fee_percent;
			position.net_profit = position.gross_profit - position.fee_entry - position.fee_exit;
			total_profit += position.net_profit;
			if (position.net_profit >= 0.0){
				win_count++;
			}
			else {
				lose_count++;
			}
			//TODO: implement -> trade_history.push_back(position);
			logger.log(position, TradeAction::SELL);
			position = TradePosition(); // Reset
			return TradeAction::SELL;
		}
	}
	return TradeAction::NONE;
}

const TradePosition& TradeEngine::get_position() const {
	return position;
}

TradePosition TradeEngine::get_current_position() const {
	return position;
}

double TradeEngine::get_latest_price() const {
	return latest_price.load();
}

double TradeEngine::get_total_profit() const {
	return total_profit;
}

int TradeEngine::get_win_count() const {
	return win_count;
}

int TradeEngine::get_lose_count() const {
	return lose_count;
}

double TradeEngine::get_winrate_percent() const {
	int total = win_count + lose_count;
	if (total == 0) return 0.0;
	return (static_cast<double>(win_count) / total) * 100.0;
}

int TradeEngine::get_total_trades() const {
	return win_count + lose_count;
}

double TradeEngine::get_average_profit() const {
	int total = get_total_trades();
	if (total == 0) return 0.0;
	return total_profit / static_cast<double>(total);
}