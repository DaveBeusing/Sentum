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
#include <iomanip>
#include <cctype>
#include <chrono>

#include <sentum/ui/ui.hpp>
#include <sentum/ui/console.hpp>


UiConsole::UiConsole() : running(false) {}

UiConsole::~UiConsole() {
	stop();
}

void UiConsole::start() {
	running = true;
	ui_thread = std::thread(&UiConsole::ui_loop, this);
	//input_thread = std::thread(&UiConsole::input_loop, this);
}

void UiConsole::stop() {
	running = false;
	if (ui_thread.joinable()) ui_thread.join();
	//if (input_thread.joinable()) input_thread.join();
}

void UiConsole::set_quote_asset(const std::string& val) {
	quote_asset = val;
}

void UiConsole::set_markets(size_t val) {
	markets = val;
}

void UiConsole::set_current_symbol(const std::string& val) {
	current_symbol = val;
}

void UiConsole::set_balance(double val) {
	balance = val;
}

void UiConsole::set_top_performer(const std::string& sym, double ret) {
	top_performer = sym;
	top_return = ret;
}

void UiConsole::set_countdown(int val) {
	countdown = val;
}

void UiConsole::set_db_path(const std::string& val) {
	db_path = val;
}

void UiConsole::set_db_size(size_t val) {
	db_size = val;
}

void UiConsole::set_start_time(const std::chrono::system_clock::time_point& t) {
	start_time = t;
}

void UiConsole::set_collector_active(bool val) {
	collector_active = val;
}

void UiConsole::set_scanner_active(bool val) {
	scanner_active = val;
}

void UiConsole::set_trader_active(bool val) {
	trader_active = val;
}

void UiConsole::set_trader_metrics(double profit, int wins, int losses, int total, double winrate, double avg_profit) {
	trader_total_profit = profit;
	trader_win_count = wins;
	trader_lose_count = losses;
	trader_total_trades = total;
	trader_winrate = winrate;
	trader_avg_profit = avg_profit;
}

void UiConsole::set_active_trade(bool is_open, double entry, double qty, double sl, double tp, double current, double profit){
	trade_open = is_open;
	entry_price = entry;
	quantity = qty;
	stop_loss = sl;
	take_profit = tp;
	current_price = current;
	current_profit = profit;
}


void UiConsole::ui_loop() {
	using namespace std::chrono_literals;
	while (running) {
		draw();
		std::this_thread::sleep_for(1s);
	}
}

void UiConsole::input_loop() {
	while (running) {
		//std::cout << "\n[q] Quit  [s] Stop Trader  [r] Restart Collector\n> ";
		char cmd;
		std::cin >> cmd;
		cmd = std::tolower(cmd);
		switch (cmd) {
			case 'q':
				if (on_exit) on_exit();
				running = false;
				break;
			case 's':
				if (on_stop_trader) on_stop_trader();
				break;
			case 'r':
				if (on_restart_collector) on_restart_collector();
				break;
		}
	}
}

void UiConsole::draw(){
	auto now = std::chrono::system_clock::now();
	auto uptime = std::chrono::duration_cast<std::chrono::seconds>(now - start_time);
	ui::clear_terminal();
	ui::show_header();
	std::cout << ui::wrap( "Engine",ui::bold()) << "\n";
	std::cout << " ├ Started:          " << ui::format_datetime(start_time) << "\n";
	std::cout << " ├ Current:          " << ui::format_datetime(now) << "\n";
	std::cout << " ├ Uptime:           " << ui::format_duration(uptime) << "\n";
	std::cout << " ├ Quote Asset:      " << ui::wrap(quote_asset, ui::bold()) << "\n";
	std::cout << " ├ Quote Balance:    " << ui::wrap_value_fixed(balance, ui::bold(), 4) << " " << ui::wrap(quote_asset, ui::bold()) << "\n";
	std::cout << " └ Collector:        " << (collector_active ? ui::wrap("CONNECTED", ui::green()) : ui::wrap("STOPPED", ui::red())) << "\n";
	std::cout << ui::wrap( "Database",ui::bold()) << "\n";
	std::cout << " ├ Database:         " << ui::wrap("ACTIVE", ui::green()) << "\n";
	std::cout << " ├ Path:             " << ( (db_path == ":memory:") ? "in-Memory" : "on-Disk" ) << "\n";
	std::cout << " └ Size:             " << (db_size / 1024) << " KB\n";
	std::cout << ui::wrap( "Scanner",ui::bold()) << "\n";
	std::cout << " ├ Scanner:          " << (scanner_active ? ui::wrap("RUNNING", ui::green()) : ui::wrap("IDLE", ui::red())) << "\n";
	std::cout << " ├ Scanning:         " << markets << " " << ui::wrap(quote_asset, ui::bold()) << " Markets\n";
	std::cout << " ├ Top Asset:        " << ui::wrap( ui::to_upper(top_performer), ui::yellow()) << " (+" << std::fixed << std::setprecision(2) << top_return << "%)\n";
	std::cout << " └ Next Scan in:     " << countdown << "s\n";
	std::cout << ui::wrap( "Trader",ui::bold()) << "\n";
	std::cout << " ├ Trader:           " << (trader_active ? ui::wrap("RUNNING", ui::green()) : ui::wrap("IDLE", ui::red())) << "\n";
	std::cout << " ├ Current Symbol:   " << ui::wrap(current_symbol.empty() ? "-" : ui::to_upper(current_symbol), ui::bold()) << "\n";
	std::cout << " ├ Total Trades:     " << trader_total_trades << "\n";
	std::cout << " ├ Win/Lose:         " << trader_win_count << "/" << trader_lose_count << "\n";
	std::cout << " ├ Winrate:          " << std::fixed << std::setprecision(2) << trader_winrate << " %\n";
	std::cout << " ├ Avg Profit:       " << trader_avg_profit << "\n";
	std::cout << " └ Total Profit:     " << trader_total_profit << "\n";

	if (trade_open) {
		std::string profit_str = current_profit >= 0 ? ui::wrap_value_fixed(current_profit, ui::bold_green(), 2) : ui::wrap_value_fixed(current_profit, ui::bold_red(), 2);
		std::cout << ui::bold() << "\nActive Trade\n" << ui::reset();
		std::cout << " ├ Entry Price:      " << ui::wrap_value_fixed(entry_price, "", 4) << "\n";
		std::cout << " ├ Quantity:         " << quantity << "\n";
		std::cout << " ├ Current Price:    " << ui::wrap_value_fixed(current_price, "", 4) << "\n";
		std::cout << " ├ Profit:           " << profit_str << "\n";
		std::cout << " ├ Stop Loss:        " << ui::wrap_value_fixed(stop_loss, "", 4) << "\n";
		std::cout << " └ Take Profit:      " << ui::wrap_value_fixed(take_profit, "", 4) << "\n";
	}

	//std::cout << "\n[q] Quit  [s] Stop Trader  [r] Restart Collector\n> ";
	std::cout << std::flush;
}