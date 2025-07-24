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
#include <thread>
#include <memory>
#include <string>

#include "sentum/utils/config.hpp"
#include "sentum/utils/database.hpp"
#include "sentum/collector/collector.hpp"
#include "sentum/scanner/scanner.hpp"
#include "sentum/trader/trader.hpp"
#include "sentum/api/binance.hpp"


class Engine {

	public:
		Engine();
		~Engine();

		void start();
		void stop();

	private:
		void init();
		void run_main_loop();
		void monitor_scanner();
		void start_trader_for( const std::string& symbol );
		void stop_trader();

		std::unique_ptr<Database> db;
		std::unique_ptr<Collector> collector;
		std::unique_ptr<SymbolScanner> scanner;
		std::unique_ptr<Trader> trader;
		std::unique_ptr<Binance> binance;

		std::atomic<bool> running;
		std::atomic<bool> trader_active;

		std::thread main_thread;
		std::thread scanner_thread;
		std::thread trader_thread;

		std::string current_symbol;
		std::vector<std::string> markets;
		Config config;

};