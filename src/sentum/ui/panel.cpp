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
#include <filesystem>

#include "sentum/ui/ui.hpp"
#include "sentum/ui/panel.hpp"


namespace ui {

	void draw_status_panel(
		const std::string& quote_asset,
		size_t markets,
		const std::string& current_symbol,
		double balance,
		const std::string& top_performer,
		double top_return,
		int countdown,
		const std::string& db_path,
		size_t db_size_bytes,
		const std::chrono::system_clock::time_point& start_time,
		bool collector_active,
		bool trader_active
	) {
		clear_terminal();
		auto now = std::chrono::system_clock::now();
		auto uptime = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now() - start_time);

		ui::show_header();

		std::cout << bold() << "Engine\n" << reset();
		std::cout << " ├ Started:			" << format_datetime(start_time) << "\n";
		std::cout << " ├ Current:			" << format_datetime(now) << "\n";
		std::cout << " ├ Uptime:			" << format_duration(uptime) << "\n";
		std::cout << " ├ Quote Balance:		" << wrap_value_fixed(balance, bold(), 4) << " " << wrap(quote_asset, bold()) << " \n";
		std::cout << " └ Collector:			" << (collector_active ? wrap("CONNECTED", green()) : wrap("STOPPED", red())) << "\n";

		std::cout << bold() << "\nScanner\n" << reset();
		std::cout << " ├ Scanning:			" << markets << " " << wrap(quote_asset, bold()) << " Markets \n";
		std::cout << " ├ Top Asset:			" << wrap(top_performer, yellow()) << " (+" << top_return << "%)\n";
		std::cout << " └ Next Scan in:		" << countdown << "s\n";

		std::cout << bold() << "\nTrader\n" << reset();
		std::cout << " ├ Trader:			" << (trader_active ? wrap("RUNNING", green()) : wrap("IDLE", red())) << "\n";
		std::cout << " └ Symbol:			" << wrap(current_symbol.empty() ? "-" : current_symbol, bold()) << "\n";

		std::cout << bold() << "\nDatabase\n" << reset();
		std::cout << " ├ Path:			" << db_path << "\n";
		std::cout << " └ Size:			" << (db_size_bytes / 1024) << " KB\n";

		std::cout << std::flush;
	}

	std::string format_duration(std::chrono::seconds s) {
		int h = s.count() / 3600;
		int m = (s.count() % 3600) / 60;
		int sec = s.count() % 60;
		std::ostringstream oss;
		if (h > 0) oss << h << "h ";
		if (m > 0 || h > 0) oss << m << "m ";
		oss << sec << "s";
		return oss.str();
	}

	std::string format_datetime(const std::chrono::system_clock::time_point& tp) {
		std::time_t time = std::chrono::system_clock::to_time_t(tp);
		std::tm* tm_ptr = std::localtime(&time);
		char buffer[20];
		std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", tm_ptr);
		return std::string(buffer);
	}


}
