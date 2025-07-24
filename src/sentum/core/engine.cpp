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

#include <chrono>
#include <iostream>

#include "sentum/core/engine.hpp"
#include "sentum/utils/config.hpp"
#include "sentum/utils/secrets.hpp"
#include "sentum/ui/ui.hpp"
#include "sentum/ui/panel.hpp"


Engine::Engine() : running(false), trader_active(false) {}

Engine::~Engine() {
	stop();
}

void Engine::start() {
		running = true;
		init();
		main_thread = std::thread(&Engine::run_main_loop, this);
}

void Engine::stop() {
	running = false;
	if (main_thread.joinable()) main_thread.join();
	if (scanner_thread.joinable()) scanner_thread.join();
	if (trader_thread.joinable()) trader_thread.join();
	if (collector) collector->stop();
	if (trader) trader->stop();
}

void Engine::init() {
	//Load global config
	config = load_config( "config/config.json" );
	//Load secrets
	Secrets secrets = load_secrets("config/secrets.json");
	if (secrets.api_key.empty() || secrets.api_secret.empty()) {
		std::cerr << "Secrets missing! please check config/secrets.json.\n";
		return;
	}
	//Initialize components
	db = std::make_unique<Database>("log/klines.sqlite3");
	binance = std::make_unique<Binance>( secrets.api_key, secrets.api_secret );
	markets = binance->get_markets_by_quote( config.quoteAsset );
	collector = std::make_unique<Collector>(*db, markets );
	scanner = std::make_unique<SymbolScanner>(*db, config.minCumulativeReturn );
	collector->start();
	std::cout << "API-Key: " << secrets.api_key.substr(0, 10) << "******\n";
}

void Engine::run_main_loop() {
	auto start_time = std::chrono::system_clock::now();
	int scanner_interval = 10;
	int countdown = scanner_interval;

	scanner_thread = std::thread(&Engine::monitor_scanner, this);

	while (running) {
		// Optional Top-Performer-Cache für Anzeige
		std::string top_asset = "-";
		double top_ret = 0.0;

		if (scanner) {
			auto top = scanner->fetch_top_performers(1, 1);
			if (!top.empty()) {
				top_asset = top[0].symbol;
				top_ret = top[0].cum_return * 100.0;
			}
		}

		double balance = binance->get_coin_balance( config.quoteAsset );
		std::string db_path = "log/klines.sqlite3";
		size_t db_size = std::filesystem::exists(db_path) ? std::filesystem::file_size(db_path) : 0;

		ui::draw_status_panel(
			config.quoteAsset,
			markets.size(),
			current_symbol,
			balance,
			top_asset,
			top_ret,
			countdown,
			db_path,
			db_size,
			start_time,
			true,            // collector active
			trader_active    // trader active
		);

		std::this_thread::sleep_for(std::chrono::seconds(1));
		if (--countdown <= 0) countdown = scanner_interval;
	}

}

void Engine::monitor_scanner() {
	using namespace std::chrono_literals;
	while (running) {
		if (!trader_active) {
			auto top = scanner->fetch_top_performers(10, 3);//fetch 3 top markets from last 10 seconds
			if (!top.empty()) {
				const auto& symbol = top[0].symbol;
				if (symbol != current_symbol) {
					start_trader_for(symbol);
					current_symbol = symbol;
				}
			}
		}
		std::this_thread::sleep_for(5s); // scanner takt
	}
}

void Engine::start_trader_for(const std::string& symbol) {
	std::cout << "Start Trader -> Symbol: " << symbol << "\n";
	trader_active = true;
	trader = std::make_unique<Trader>(symbol, *db, *binance);
	trader_thread = std::thread([this] {
		trader->run();
		trader_active = false;
	});
}

void Engine::stop_trader() {
	if (trader) {
		trader->stop();
		if (trader_thread.joinable()) trader_thread.join();
	}
	trader.reset();
	trader_active = false;
}