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
//#include "sentum/utils/config.hpp"
#include "sentum/utils/secrets.hpp"


std::vector<std::string> get_trading_symbols() {
	return { "BTCUSDC", "ETHUSDC", "SHIBUSDC" };
}


Engine::Engine() : running(false) {}

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
	//TODO

	//Load secrets
	Secrets secrets = load_secrets("config/secrets.json");
	if (secrets.api_key.empty() || secrets.api_secret.empty()) {
		std::cerr << "Secrets missing! please check config/secrets.json.\n";
		//return 1;
	}
	std::cout << "API-Key: " << secrets.api_key.substr(0, 6) << "****\n";

	//Initialize components
	db = std::make_unique<Database>("log/klines.sqlite3");
	binance = std::make_unique<Binance>( secrets.api_key, secrets.api_secret );
	collector = std::make_unique<Collector>(*db, get_trading_symbols());
	scanner = std::make_unique<SymbolScanner>(*db);
	collector->start();

	double balance = binance->get_coin_balance("USDC");
	std::cout << "Current Wallet: " << balance << " USDC\n";
}

void Engine::run_main_loop() {
	scanner_thread = std::thread(&Engine::monitor_scanner, this);
}

void Engine::monitor_scanner() {
	using namespace std::chrono_literals;
	while (running) {
		auto top = scanner->fetch_top_performers(1, 1);
		if (!top.empty()) {
			const auto& symbol = top[0].symbol;
			if (symbol != current_symbol) {
				stop_trader();
				start_trader_for(symbol);
				current_symbol = symbol;
			}
		}
		std::this_thread::sleep_for(5s); // Scanner-Takt
	}
}

void Engine::start_trader_for(const std::string& symbol) {
	std::cout << "Start Trader -> Symbol: " << symbol << "\n";
	trader = std::make_unique<Trader>(symbol, *db, *binance);
	trader_thread = std::thread([this] {
		trader->run();
	});
}

void Engine::stop_trader() {
	if (trader) {
		trader->stop();
		if (trader_thread.joinable()) trader_thread.join();
	}
	trader.reset();
}