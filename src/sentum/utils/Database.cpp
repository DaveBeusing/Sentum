/****
 * Copyright (C) 2025 Dave Beusing <david.beusing@gmail.com>
 * MIT License - https://opensource.org/license/mit/
 */

#include <iostream>
#include <stdexcept>

#include "sentum/utils/Database.hpp"

Database::Database(const std::string& db_path) : db(nullptr) {
	if (sqlite3_open(db_path.c_str(), &db) != SQLITE_OK) {
		const std::string message = db ? sqlite3_errmsg(db) : "unknown SQLite error";
		if (db) {
			sqlite3_close(db);
			db = nullptr;
		}
		throw std::runtime_error("Failed to open database: " + message);
	}

	sqlite3_busy_timeout(db, 5000);
	if (!ensure_table()) {
		throw std::runtime_error("Failed to initialize database schema");
	}
}

Database::~Database() {
	if (db) sqlite3_close(db);
}

bool Database::ensure_table() {
	const char* sql = "CREATE TABLE IF NOT EXISTS klines ( symbol TEXT NOT NULL, timestamp INTEGER NOT NULL, "
		"open REAL, high REAL, low REAL, close REAL, volume REAL, PRIMARY KEY (symbol, timestamp)"
		");";
	const char* index = "CREATE INDEX IF NOT EXISTS idx_klines_symbol_ts ON klines(symbol, timestamp DESC);";
	char* err = nullptr;
	if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
		std::cerr << " Database table error: " << (err ? err : "unknown") << "\n";
		sqlite3_free(err);
		return false;
	}
	if (sqlite3_exec(db, index, nullptr, nullptr, &err) != SQLITE_OK) {
		std::cerr << " Database index error: " << (err ? err : "unknown") << "\n";
		sqlite3_free(err);
		return false;
	}
	return true;
}

bool Database::save_klines(const std::string& symbol, const std::vector<Kline>& klines) {
	if (klines.empty()) return false;
	const char* insert = "INSERT OR IGNORE INTO klines (symbol, timestamp, open, high, low, close, volume) VALUES (?, ?, ?, ?, ?, ?, ?);";
	if (sqlite3_exec(db, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr) != SQLITE_OK) return false;

	sqlite3_stmt* stmt = nullptr;
	if (sqlite3_prepare_v2(db, insert, -1, &stmt, nullptr) != SQLITE_OK) {
		sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
		std::cerr << " Kline insert error: " << sqlite3_errmsg(db) << "\n";
		return false;
	}

	bool success = true;
	for (const auto& k : klines) {
		sqlite3_bind_text(stmt, 1, symbol.c_str(), -1, SQLITE_TRANSIENT);
		sqlite3_bind_int64(stmt, 2, k.timestamp);
		sqlite3_bind_double(stmt, 3, k.open);
		sqlite3_bind_double(stmt, 4, k.high);
		sqlite3_bind_double(stmt, 5, k.low);
		sqlite3_bind_double(stmt, 6, k.close);
		sqlite3_bind_double(stmt, 7, k.volume);
		if (sqlite3_step(stmt) != SQLITE_DONE) {
			std::cerr << " Insert failed: " << sqlite3_errmsg(db) << "\n";
			success = false;
			break;
		}
		sqlite3_reset(stmt);
		sqlite3_clear_bindings(stmt);
	}
	sqlite3_finalize(stmt);
	sqlite3_exec(db, success ? "COMMIT;" : "ROLLBACK;", nullptr, nullptr, nullptr);
	return success;
}

std::vector<Kline> Database::load_klines(const std::string& symbol, int limit) {
	std::vector<Kline> result;
	const char* sql = "SELECT timestamp, open, high, low, close, volume FROM klines WHERE symbol = ? ORDER BY timestamp DESC LIMIT ?";
	sqlite3_stmt* stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, symbol.c_str(), -1, SQLITE_TRANSIENT);
		sqlite3_bind_int(stmt, 2, limit);
		while (sqlite3_step(stmt) == SQLITE_ROW) {
			Kline kline;
			kline.timestamp = sqlite3_column_int64(stmt, 0);
			kline.open = sqlite3_column_double(stmt, 1);
			kline.high = sqlite3_column_double(stmt, 2);
			kline.low = sqlite3_column_double(stmt, 3);
			kline.close = sqlite3_column_double(stmt, 4);
			kline.volume = sqlite3_column_double(stmt, 5);
			result.push_back(kline);
		}
	} else {
		std::cerr << "load_klines error: " << sqlite3_errmsg(db) << "\n";
	}
	if (stmt) sqlite3_finalize(stmt);
	return result;
}
