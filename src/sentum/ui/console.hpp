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

#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <functional>


class UiConsole {

	public:
		UiConsole();
		~UiConsole();

		void start();
		void stop();

		void set_quote_asset(const std::string&);
		void set_markets(size_t);
		void set_current_symbol(const std::string&);
		void set_balance(double);
		void set_top_performer(const std::string&, double);
		void set_countdown(int);
		void set_db_path(const std::string&);
		void set_db_size(size_t);
		void set_start_time(const std::chrono::system_clock::time_point&);
		void set_collector_active(bool);
		void set_scanner_active(bool);
		void set_trader_active(bool);

		std::function<void()> on_stop_trader;
		std::function<void()> on_restart_collector;
		std::function<void()> on_exit;

	private:
		std::atomic<bool> running;
		std::thread ui_thread;
		std::thread input_thread;

		// Statuswerte
		std::string quote_asset;
		size_t markets = 0;
		std::string current_symbol;
		double balance = 0.0;
		std::string top_performer;
		double top_return = 0.0;
		int countdown = 0;
		std::string db_path;
		size_t db_size = 0;
		std::chrono::system_clock::time_point start_time;
		bool collector_active = false;
		bool scanner_active = false;
		bool trader_active = false;

		void ui_loop();
		void input_loop();
		void draw();

};