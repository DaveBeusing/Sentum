#pragma once

#include <chrono>
#include <stdexcept>
#include <string>

#include <sqlite3.h>
#include <sentum/trader/types/TradePosition.hpp>

class TradeHistoryRepository {
public:
    explicit TradeHistoryRepository(const std::string& path) {
        if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) throw std::runtime_error("Failed to open trade history database");
        const char* ddl = "CREATE TABLE IF NOT EXISTS trades (id INTEGER PRIMARY KEY AUTOINCREMENT, symbol TEXT NOT NULL, strategy TEXT NOT NULL, signal_reason TEXT NOT NULL, risk_approved INTEGER NOT NULL, risk_reason TEXT NOT NULL, signal_ts INTEGER NOT NULL, entry_ts INTEGER NOT NULL, exit_ts INTEGER NOT NULL, reference_price REAL NOT NULL, entry_price REAL NOT NULL, exit_price REAL NOT NULL, quantity REAL NOT NULL, gross_profit REAL NOT NULL, fees REAL NOT NULL, net_profit REAL NOT NULL, exit_reason TEXT NOT NULL, simulated INTEGER NOT NULL);";
        if (sqlite3_exec(db_, ddl, nullptr, nullptr, nullptr) != SQLITE_OK) throw std::runtime_error("Failed to create trades table");
        const char* sql = "INSERT INTO trades(symbol,strategy,signal_reason,risk_approved,risk_reason,signal_ts,entry_ts,exit_ts,reference_price,entry_price,exit_price,quantity,gross_profit,fees,net_profit,exit_reason,simulated) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";
        if (sqlite3_prepare_v2(db_, sql, -1, &insert_, nullptr) != SQLITE_OK) throw std::runtime_error("Failed to prepare trade history insert");
    }

    ~TradeHistoryRepository() { if (insert_) sqlite3_finalize(insert_); if (db_) sqlite3_close(db_); }

    void save(const TradePosition& p) {
        auto ms = [](auto tp) { return std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count(); };
        sqlite3_reset(insert_); sqlite3_clear_bindings(insert_);
        sqlite3_bind_text(insert_,1,p.symbol.c_str(),-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(insert_,2,p.strategy.c_str(),-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(insert_,3,p.signal_reason.c_str(),-1,SQLITE_TRANSIENT);
        sqlite3_bind_int(insert_,4,p.risk_approved ? 1 : 0);
        sqlite3_bind_text(insert_,5,p.risk_reason.c_str(),-1,SQLITE_TRANSIENT);
        sqlite3_bind_int64(insert_,6,ms(p.signal_time)); sqlite3_bind_int64(insert_,7,ms(p.entry_time)); sqlite3_bind_int64(insert_,8,ms(p.exit_time));
        sqlite3_bind_double(insert_,9,p.reference_price); sqlite3_bind_double(insert_,10,p.entry_price); sqlite3_bind_double(insert_,11,p.exit_price);
        sqlite3_bind_double(insert_,12,p.quantity); sqlite3_bind_double(insert_,13,p.gross_profit); sqlite3_bind_double(insert_,14,p.fee_entry+p.fee_exit);
        sqlite3_bind_double(insert_,15,p.net_profit); sqlite3_bind_text(insert_,16,p.close_reason.c_str(),-1,SQLITE_TRANSIENT); sqlite3_bind_int(insert_,17,p.simulated ? 1 : 0);
        if (sqlite3_step(insert_) != SQLITE_DONE) throw std::runtime_error("Failed to persist trade history");
    }

private:
    sqlite3* db_ = nullptr;
    sqlite3_stmt* insert_ = nullptr;
};
