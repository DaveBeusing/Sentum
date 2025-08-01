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

#include <sentum/core/ExecutionEngine.hpp>
#include <sentum/ui/console.hpp>


ExecutionEngine::ExecutionEngine() : running(false), collector_active(false), scanner_active(false), trader_active(false), logger("log/core.log") {
	logger.start();
}

ExecutionEngine::~ExecutionEngine() {
	stop();
	logger.stop();
}

bool ExecutionEngine::is_running() const {
	return running.load();
}

void ExecutionEngine::start() {
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
		main_thread = std::thread(&ExecutionEngine::run_main_loop, this);
}

void ExecutionEngine::stop() {
	std::cout << "[Engine] stop() aufgerufen\n";
	running = false;
	if (main_thread.joinable()) main_thread.join();
	if (scanner_thread.joinable()) scanner_thread.join();
	if (trader_thread.joinable()) trader_thread.join();
	if (collector) collector->stop();
	if (trader) trader->stop();
	if (ui) ui->stop();
	if (ui_thread.joinable()) ui_thread.join();
	ui.reset();
}

void ExecutionEngine::init_config() {

	try{
		config = load_config("config/config.json");
	} catch(const std::exception& e) {
		logger.log("[ERROR] Failed to load config/config.json: " + std::string(e.what()));
		throw std::runtime_error("Failed to load config/config.json!");
	}

	try{
		secrets = load_secrets("config/secrets.json");
		if (secrets.api_key.empty() || secrets.api_secret.empty()) {
			throw std::runtime_error("Missing API keys in secrets.json!");
		}
	} catch(const std::exception& e) {
		logger.log("[ERROR] Failed to load secrets.json: " + std::string(e.what()));
		throw std::runtime_error("Failed to load secrets.json!");
	}

	if( config.paperTrading ){
		logger.log("[INFO] Running in PAPER TRADING mode");
	} else {
		logger.log("[INFO] Running in LIVE mode");
	}

}

void ExecutionEngine::init_components() {
	db_path = "log/klines.sqlite3";

	try {
		binance = std::make_unique<BinanceRestClient>(secrets.api_key, secrets.api_secret);
		markets = binance->get_markets_by_quote(config.quoteAsset);
		quote_balance = binance->get_coin_balance(config.quoteAsset);
	} catch(const std::exception& e) {
		logger.log("[ERROR] Failed to initialize BinanceRestClient or dependencies: " + std::string(e.what()));
		throw std::runtime_error("Failed to initialize BinanceRestClient or dependencies of it!");
	}

	try {
		db = std::make_unique<Database>(db_path);
	} catch(const std::exception& e) {
		logger.log("[ERROR] Failed to initialize Database: " + std::string(e.what()));
		throw std::runtime_error("Failed to initialize Database!");
	}

	try {
		collector = std::make_unique<Collector>(*db, markets);
		collector_active = true;
		collector->start();
	} catch(const std::exception& e) {
		logger.log("[ERROR] Failed to initialize or start Collector: " + std::string(e.what()));
		throw std::runtime_error("Failed to initialize or start Collector!");
	}

	try {
		scanner = std::make_unique<SymbolScanner>(*db, config.minCumulativeReturn);
		scanner_active = true;
	} catch(const std::exception& e) {
		logger.log("[ERROR] Failed to initialize SymbolScanner: " + std::string(e.what()));
		throw std::runtime_error("Failed to initialize SymbolScanner!");
	}

	try {
		ui = std::make_unique<UiConsole>();
	} catch(const std::exception& e) {
		logger.log("[ERROR] Failed to initialize UiConsole: " + std::string(e.what()));
		throw std::runtime_error("Failed to initialize UiConsole!");
	}

}

void ExecutionEngine::init() {
	init_config();
	init_components();

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

void ExecutionEngine::run_main_loop() {
	scanner_thread = std::thread(&ExecutionEngine::monitor_scanner, this);
	int scanner_interval = 10;
	int countdown = scanner_interval;
	try {
		while (running) {
			std::string top_asset = "-";
			double top_ret = 0.0;
			if (scanner) {
				auto top = scanner->fetch_top_performers(60, 3);
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
				double price = trader->get_latest_price();
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
	} catch( const std::exception& e ) {
		logger.log("[ERROR] ExecutionEngine::run_main_loop: " + std::string(e.what()));
	}
}

void ExecutionEngine::monitor_scanner() {
	using namespace std::chrono_literals;
	try {
		while (running) {
			if (!trader_active) {
				auto top = scanner->fetch_top_performers(30, 3);//fetch 3 top markets from last 30 seconds
				if (!top.empty()) {
					std::lock_guard<std::mutex> lock(symbol_mutex);
					const auto& symbol = top[0].symbol;
					if (symbol != current_symbol) {
						current_symbol = symbol;
						start_trader_for(symbol);
						ui->set_current_symbol(current_symbol);
					}
				}
			}
			std::this_thread::sleep_for(5s); // scanner takt
		}
	} catch(const std::exception& e){
		logger.log("[ERROR] ExecutionEngine::monitor_scanner: " + std::string(e.what()));
	}
}

void ExecutionEngine::start_trader_for(const std::string& symbol) {
	trader_active = true;
	trader = std::make_unique<TradeEngine>(symbol, *binance, config.paperTrading );
	trader_thread = std::thread([this] {
		try{
			trader->run();
		} catch(const std::exception& e){
			logger.log("[ERROR] ExecutionEngine::start_trader_for: " + std::string(e.what()));
		}
		trader_active = false;
	});
}

void ExecutionEngine::stop_trader() {
	if (trader) {
		trader->stop();
		if (trader_thread.joinable()) trader_thread.join();
	}
	trader.reset();
	trader_active = false;
}