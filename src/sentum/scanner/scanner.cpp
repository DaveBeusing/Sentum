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

#include "sentum/scanner/scanner.hpp"
#include "sentum/utils/database.hpp"
#include <sqlite3.h>
#include <iostream>
#include <cmath>
#include <algorithm>

SymbolScanner::SymbolScanner(Database& db_) : database(db_) {}

std::vector<SymbolPerformance> SymbolScanner::fetch_top_performers(int lookback, int max_symbols) {
	std::vector<SymbolPerformance> result;
	sqlite3* db = database.get_connection();
	std::string sql = "SELECT name FROM sqlite_master WHERE type='table' AND name LIKE 'klines_%';";
	sqlite3_stmt* stmt;
	if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
		std::cerr << " Error SymbolScanner::fetch_top_performers: " << sqlite3_errmsg(db) << "\n";
		return result;
	}
	std::vector<std::string> symbols;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		std::string table = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
		if (table.rfind("klines_", 0) == 0) {
			symbols.push_back(table.substr(7)); // remove table prefix
		}
	}
	sqlite3_finalize(stmt);
	for (const auto& symbol : symbols) {
		std::string table = "klines_" + symbol;
		std::string query = "SELECT close FROM " + table + " ORDER BY timestamp DESC LIMIT " + std::to_string(lookback);		
		if (sqlite3_prepare_v2(db, query.c_str(), -1, &stmt, nullptr) != SQLITE_OK) continue;
		std::vector<double> closes;
		while (sqlite3_step(stmt) == SQLITE_ROW) {
			closes.push_back(sqlite3_column_double(stmt, 0));
		}
		sqlite3_finalize(stmt);
		if (closes.size() < 2) continue;
		double cum_return = 1.0;
		for (size_t i = 1; i < closes.size(); ++i) {
			double prev = closes[i - 1];
			if (prev == 0.0) continue;
			double pct = (closes[i] - prev) / prev;
			cum_return *= (1.0 + pct);
		}
		cum_return -= 1.0;
		if (cum_return > 0.0) {
			result.push_back({symbol, std::round(cum_return * 1e8) / 1e8});
		}
	}
	std::sort(result.begin(), result.end(), [](const SymbolPerformance& a, const SymbolPerformance& b) {
		return a.cum_return > b.cum_return;
	});
	if (max_symbols > 0 && static_cast<int>(result.size()) > max_symbols) {
		result.resize(max_symbols);
	}
	return result;
}
