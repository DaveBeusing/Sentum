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
#include <chrono>

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
	);

	std::string format_duration(std::chrono::seconds s);
	std::string format_datetime(const std::chrono::system_clock::time_point& tp);

}