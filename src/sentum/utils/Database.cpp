#include <algorithm>
#include <iostream>
#include <stdexcept>

#include <sentum/utils/Database.hpp>

Database::Database(const std::string& db_path) {
    if (sqlite3_open_v2(db_path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr) != SQLITE_OK) {
        const std::string message = db ? sqlite3_errmsg(db) : "unknown SQLite error";
        if (db) sqlite3_close(db);
        db = nullptr;
        throw std::runtime_error("Failed to open database: " + message);
    }
    sqlite3_busy_timeout(db, 5000);
    exec_or_throw("PRAGMA journal_mode=WAL;");
    exec_or_throw("PRAGMA synchronous=NORMAL;");
    exec_or_throw("PRAGMA temp_store=MEMORY;");
    exec_or_throw("PRAGMA wal_autocheckpoint=1000;");
    if (!ensure_table()) throw std::runtime_error("Failed to initialize database schema");

    const char* sql =
        "INSERT INTO klines(symbol,timestamp,open,high,low,close,volume) VALUES(?,?,?,?,?,?,?) "
        "ON CONFLICT(symbol,timestamp) DO UPDATE SET open=excluded.open,high=excluded.high,"
        "low=excluded.low,close=excluded.close,volume=excluded.volume;";
    if (sqlite3_prepare_v2(db, sql, -1, &upsert_stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare kline UPSERT: " + std::string(sqlite3_errmsg(db)));
    }
}

Database::~Database() {
    if (upsert_stmt) sqlite3_finalize(upsert_stmt);
    if (db) sqlite3_close(db);
}

void Database::exec_or_throw(const char* sql) {
    char* error = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &error) != SQLITE_OK) {
        const std::string message = error ? error : sqlite3_errmsg(db);
        sqlite3_free(error);
        throw std::runtime_error(message);
    }
}

bool Database::ensure_table() {
    try {
        exec_or_throw("CREATE TABLE IF NOT EXISTS klines(symbol TEXT NOT NULL,timestamp INTEGER NOT NULL,open REAL,high REAL,low REAL,close REAL,volume REAL,PRIMARY KEY(symbol,timestamp));");
        exec_or_throw("CREATE INDEX IF NOT EXISTS idx_klines_symbol_ts ON klines(symbol,timestamp DESC);");
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Database schema error: " << e.what() << '\n';
        return false;
    }
}

bool Database::bind_and_step(const std::string& symbol, const Kline& kline) {
    sqlite3_reset(upsert_stmt);
    sqlite3_clear_bindings(upsert_stmt);
    sqlite3_bind_text(upsert_stmt, 1, symbol.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(upsert_stmt, 2, kline.timestamp);
    sqlite3_bind_double(upsert_stmt, 3, kline.open);
    sqlite3_bind_double(upsert_stmt, 4, kline.high);
    sqlite3_bind_double(upsert_stmt, 5, kline.low);
    sqlite3_bind_double(upsert_stmt, 6, kline.close);
    sqlite3_bind_double(upsert_stmt, 7, kline.volume);
    return sqlite3_step(upsert_stmt) == SQLITE_DONE;
}

bool Database::save_kline_batch(const std::vector<std::pair<std::string, Kline>>& batch) {
    if (batch.empty()) return true;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) return false;
    for (const auto& item : batch) {
        if (!bind_and_step(item.first, item.second)) {
            sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
            return false;
        }
    }
    return sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr) == SQLITE_OK;
}

bool Database::save_klines(const std::string& symbol, const std::vector<Kline>& klines) {
    std::vector<std::pair<std::string, Kline>> batch;
    batch.reserve(klines.size());
    for (const auto& kline : klines) batch.emplace_back(symbol, kline);
    return save_kline_batch(batch);
}

std::vector<Kline> Database::load_klines(const std::string& symbol, int limit) {
    std::vector<Kline> result;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT timestamp,open,high,low,close,volume FROM klines WHERE symbol=? ORDER BY timestamp DESC LIMIT ?;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return result;
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
    sqlite3_finalize(stmt);
    std::reverse(result.begin(), result.end());
    return result;
}
