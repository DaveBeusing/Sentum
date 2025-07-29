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
#include <cmath>
#include <algorithm>
#include <sqlite3.h>

#include <sentum/scanner/SymbolScanner.hpp>
#include <sentum/utils/database.hpp>

constexpr double ROUND_FACTOR = 1e8;

SymbolScanner::SymbolScanner(Database& db_, double threshold) : database(db_), min_return_threshold(threshold) {}

std::vector<SymbolPerformance> SymbolScanner::fetch_top_performers(int lookback, int max_symbols) {
	std::vector<SymbolPerformance> result;
	sqlite3* db = database.get_connection();
	const char* sql = "SELECT DISTINCT symbol FROM klines;";
	sqlite3_stmt* stmt;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		//TODO: logging
		return result;
	}
	std::vector<std::string> symbols;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		const unsigned char* sym = sqlite3_column_text(stmt, 0);
		if (sym) {
			symbols.emplace_back(reinterpret_cast<const char*>(sym));
		}
	}
	sqlite3_finalize(stmt);
	// calculate cumulative return for each symbol
	const std::string query = "SELECT close FROM ( SELECT close FROM klines WHERE symbol = ? ORDER BY timestamp ASC LIMIT ? );";
	for (const auto& symbol : symbols) {
		if (sqlite3_prepare_v2(db, query.c_str(), -1, &stmt, nullptr) != SQLITE_OK){
			//TODO: logging
			continue;
		}
		sqlite3_bind_text(stmt, 1, symbol.c_str(), -1, SQLITE_STATIC);
		sqlite3_bind_int(stmt, 2, lookback);
		double first = -1.0, last = -1.0;
		int count = 0;
		while (sqlite3_step(stmt) == SQLITE_ROW) {
			double close = sqlite3_column_double(stmt, 0);
			if (count == 0) first = close;
			last = close;
			count++;
		}
		sqlite3_finalize(stmt);
		if (count < 2 || first <= 0.0) continue;
		double cum_return = (last - first) / first;
		if (cum_return > min_return_threshold) {
			result.push_back({symbol, std::round(cum_return * ROUND_FACTOR) / ROUND_FACTOR});
		}
	}
	std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
		return a.cum_return > b.cum_return;
	});
	if (max_symbols > 0 && static_cast<int>(result.size()) > max_symbols) {
		result.resize(max_symbols);
	}
	return result;
}
