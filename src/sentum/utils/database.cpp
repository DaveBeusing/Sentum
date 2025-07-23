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
#include <filesystem>

#include "sentum/utils/database.hpp"


Database::Database(const std::string& db_path) : db(nullptr) {
	if (std::filesystem::exists(db_path)) {
		std::filesystem::remove(db_path);
	}
	if (sqlite3_open(db_path.c_str(), &db) != SQLITE_OK) {
		std::cerr << " Error open database: " << sqlite3_errmsg(db) << "\n";
	}
}

Database::~Database() {
	if (db) sqlite3_close(db);
}

bool Database::ensure_table(const std::string& symbol) {
	std::string table = "klines_" + symbol;
	std::string sql = "CREATE TABLE IF NOT EXISTS " + table + " ("
		"id INTEGER PRIMARY KEY AUTOINCREMENT, "
		"timestamp INTEGER NOT NULL UNIQUE, "
		"open REAL, "
		"high REAL, "
		"low REAL, "
		"close REAL, "
		"volume REAL"
		");";
	char* err = nullptr;
	if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
		std::cerr << " Database table error: " << err << "\n";
		sqlite3_free(err);
		return false;
	}
	return true;
}

bool Database::save_klines(const std::string& symbol, const std::vector<Kline>& klines) {
	if( klines.empty() ) return false;
	if( !ensure_table( symbol ) ) return false;
	std::string table = "klines_" + symbol;
	std::string insert = "INSERT OR IGNORE INTO " + table + " (timestamp, open, high, low, close, volume) VALUES (?, ?, ?, ?, ?, ?);";
	sqlite3_exec(db, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);
	sqlite3_stmt* stmt;
	if (sqlite3_prepare_v2(db, insert.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
		std::cerr << " Error prepare klines insert: " << sqlite3_errmsg(db) << "\n";
		return false;
	}
	for (const auto& k : klines) {
		sqlite3_bind_int64(stmt, 1, k.timestamp);
		sqlite3_bind_double(stmt, 2, k.open);
		sqlite3_bind_double(stmt, 3, k.high);
		sqlite3_bind_double(stmt, 4, k.low);
		sqlite3_bind_double(stmt, 5, k.close);
		sqlite3_bind_double(stmt, 6, k.volume);
		if (sqlite3_step(stmt) != SQLITE_DONE) {
			std::cerr << " Error insert klines: " << sqlite3_errmsg(db) << "\n";
		}
		sqlite3_reset(stmt);
	}
	sqlite3_finalize(stmt);
	sqlite3_exec(db, "END TRANSACTION;", nullptr, nullptr, nullptr);
	return true;
}

std::vector<Kline> Database::load_klines(const std::string& symbol, int limit) {
	std::vector<Kline> result;
	std::string table = "klines_" + symbol;
	std::string sql = "SELECT timestamp, open, high, low, close, volume FROM " + table + " ORDER BY timestamp DESC LIMIT " + std::to_string(limit);
	sqlite3_stmt* stmt;
	if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
		while (sqlite3_step(stmt) == SQLITE_ROW) {
			Kline kline;
			kline.timestamp = sqlite3_column_int64(stmt, 0);
			kline.open      = sqlite3_column_double(stmt, 1);
			kline.high      = sqlite3_column_double(stmt, 2);
			kline.low       = sqlite3_column_double(stmt, 3);
			kline.close     = sqlite3_column_double(stmt, 4);
			kline.volume    = sqlite3_column_double(stmt, 5);
			result.push_back(kline);
		}
	} else {
		std::cerr << " Error load_klines: " << sqlite3_errmsg(db) << "\n";
	}
	sqlite3_finalize(stmt);
	return result;
}