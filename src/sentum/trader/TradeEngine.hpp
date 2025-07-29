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

#include <atomic>
#include <string>
#include <chrono>
#include <thread>

#include <sentum/trader/types/RiskConfig.hpp>
#include <sentum/trader/types/TradePosition.hpp>
#include <sentum/trader/types/TradeAction.hpp>
#include <sentum/trader/utils/RiskConfigLoader.hpp>
#include <sentum/trader/utils/TradeLogger.hpp>
#include <sentum/api/binance.hpp>
#include <sentum/api/websocket.hpp>


class TradeEngine {

	public:

		TradeEngine(const std::string& symbol, Binance& binance);
		void run();
		void stop();

		TradeAction evaluate(double price);
		const TradePosition& get_position() const;//??
		TradePosition get_current_position() const;
		double get_latest_price() const;

		double get_total_profit() const;
		int get_win_count() const;
		int get_lose_count() const;
		double get_winrate_percent() const;
		int get_total_trades() const;
		double get_average_profit() const;

	private:
		std::string symbol;
		Binance& api;
		std::atomic<bool> running;

		RiskConfig risk;
		TradePosition position;

		TradeLogger logger;

		double total_profit = 0.0;
		int win_count = 0;
		int lose_count = 0;

		std::unique_ptr<Websocket> price_stream;
		std::atomic<double> latest_price = 0.0;

};