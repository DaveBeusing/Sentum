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

#include <sentum/core/engine.hpp>
#include <sentum/utils/config.hpp>
#include <sentum/utils/secrets.hpp>
#include <sentum/ui/console.hpp>


Engine::Engine() : running(false), collector_active(false), scanner_active(false), trader_active(false) {}

Engine::~Engine() {
	stop();
}

bool Engine::is_running() const {
	return running.load();
}

void Engine::start() {
		running = true;
		init();
		ui->on_exit = [this]() { stop(); };
		ui->on_stop_trader = [this]() { stop_trader(); };
		ui->on_restart_collector = [this]() {
			if (collector) {
				collector->stop();
				collector->start();
				ui->set_collector_active(true);
			}
		};
		ui_thread = std::thread([this] { ui->start(); });
		main_thread = std::thread(&Engine::run_main_loop, this);
}

void Engine::stop() {
	running = false;
	if (main_thread.joinable()) main_thread.join();
	if (scanner_thread.joinable()) scanner_thread.join();
	if (trader_thread.joinable()) trader_thread.join();
	if (collector) collector->stop();
	if (trader) trader->stop();
	if (ui) ui->stop();
	if (ui_thread.joinable()) ui_thread.join();
	ui.reset(); //??? clean
}

void Engine::init() {
	config = load_config( "config/config.json" );
	Secrets secrets = load_secrets("config/secrets.json");
	if (secrets.api_key.empty() || secrets.api_secret.empty()) {
		std::cerr << "Secrets missing! please check config/secrets.json.\n";
		return;
	}
	ui = std::make_unique<UiConsole>();
	db_path = "log/klines.sqlite3";
	db = std::make_unique<Database>( db_path );
	binance = std::make_unique<Binance>( secrets.api_key, secrets.api_secret );
	markets = binance->get_markets_by_quote( config.quoteAsset );
	collector = std::make_unique<Collector>(*db, markets );
	collector_active = true;
	scanner = std::make_unique<SymbolScanner>(*db, config.minCumulativeReturn );
	scanner_active = true;
	collector->start();
	start_time = std::chrono::system_clock::now();
	quote_balance = binance->get_coin_balance(config.quoteAsset);
	db_size = std::filesystem::exists(db_path) ? std::filesystem::file_size(db_path) : 0;

	std::cout << "API-Key: " << secrets.api_key.substr(0, 10) << "******\n";


	start_time = std::chrono::system_clock::now();

	ui->set_collector_active(collector_active);
	ui->set_scanner_active(scanner_active);
	ui->set_trader_active(trader_active);

	ui->set_balance(quote_balance);
	ui->set_quote_asset(config.quoteAsset);
	ui->set_markets(markets.size());
	ui->set_start_time(start_time);
	ui->set_db_path( db_path );

}

void Engine::run_main_loop() {
	scanner_thread = std::thread(&Engine::monitor_scanner, this);
	int scanner_interval = 10;
	int countdown = scanner_interval;
	while (running) {
		std::string top_asset = "-";
		double top_ret = 0.0;
		if (scanner) {
			auto top = scanner->fetch_top_performers(10, 3);
			if (!top.empty()) {
				top_asset = top[0].symbol;
				top_ret = top[0].cum_return * 100.0;
			}
		}
		db_size = std::filesystem::exists(db_path) ? std::filesystem::file_size(db_path) : 0;

		ui->set_top_performer(top_asset, top_ret);
		ui->set_countdown(countdown);
		ui->set_db_size(db_size);
		ui->set_collector_active(collector_active);
		ui->set_scanner_active(scanner_active);
		ui->set_trader_active(trader_active);
		ui->set_current_symbol(current_symbol);

		if (trader) {
			ui->set_trader_metrics(
				trader->get_total_profit(),
				trader->get_win_count(),
				trader->get_lose_count(),
				trader->get_total_trades(),
				trader->get_winrate_percent(),
				trader->get_average_profit()
			);
		}

		if (trader && trader->get_position().open) {
			const auto& pos = trader->get_position();
			double price = 1.0;//binance->get_price(pos.symbol);  // Preis-Update
			double profit = (price - pos.entry_price) * pos.quantity;

			ui->set_active_trade(
				pos.open,
				pos.entry_price,
				pos.quantity,
				pos.stop_loss_price,
				pos.take_profit_price,
				price,
				profit
			);
		} else {
			ui->set_active_trade(false, 0, 0, 0, 0, 0, 0);
		}

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
					current_symbol = symbol; // is it safe???
					ui->set_current_symbol(current_symbol);
				}
			}
		}
		std::this_thread::sleep_for(5s); // scanner takt
	}
}

void Engine::start_trader_for(const std::string& symbol) {
	trader_active = true;
	trader = std::make_unique<Trader>(symbol, *binance);
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