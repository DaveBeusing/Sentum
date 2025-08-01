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

#include <memory>
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <chrono>
#include <mutex>

#include <sentum/utils/ConfigLoader.hpp>
#include <sentum/utils/SecretsLoader.hpp>
#include <sentum/utils/AsyncLogger.hpp> 
#include <sentum/utils/Database.hpp>
#include <sentum/api/BinanceRestClient.hpp>
#include <sentum/collector/Collector.hpp>
#include <sentum/scanner/SymbolScanner.hpp>
#include <sentum/trader/TradeEngine.hpp>
#include <sentum/ui/UiConsole.hpp>


class ExecutionEngine {

	public:
		ExecutionEngine();
		~ExecutionEngine();

		void start();
		void stop();

		bool is_running() const;

	private:
		// Threads 
		std::thread main_thread, ui_thread, scanner_thread, trader_thread;

		// Status flags
		std::atomic<bool> running, collector_active, scanner_active, trader_active;

		// Synchronisation
		std::mutex symbol_mutex;

		// Components
		std::unique_ptr<Database> db;
		std::unique_ptr<BinanceRestClient> binance;
		std::unique_ptr<Collector> collector;
		std::unique_ptr<SymbolScanner> scanner;
		std::unique_ptr<TradeEngine> trader;
		std::unique_ptr<UiConsole> ui;

		// State
		std::string current_symbol;
		std::vector<MarketInfo> markets;
		double quote_balance;
		std::string db_path;
		size_t db_size;

		std::chrono::system_clock::time_point start_time;
		Config config;
		Secrets secrets;
		AsyncLogger logger;

		//Methods
		void init();
		void init_config();
		void init_components();
		void run_main_loop();
		void monitor_scanner();
		void start_trader_for( const std::string& symbol );
		void stop_trader();

};