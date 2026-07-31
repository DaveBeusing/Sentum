#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>

#include <sqlite3.h>
#include <sentum/trader/order/OrderTypes.hpp>

namespace sentum::order {

class OrderEventRepository {
public:
    explicit OrderEventRepository(const std::string& path) {
        if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) throw std::runtime_error("Failed to open order-event database");
        sqlite3_busy_timeout(db_, 5000);
        const char* ddl =
            "CREATE TABLE IF NOT EXISTS order_events ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, client_order_id TEXT NOT NULL, exchange_order_id INTEGER NOT NULL, "
            "symbol TEXT NOT NULL, side TEXT NOT NULL, state TEXT NOT NULL, source TEXT NOT NULL, "
            "requested_qty REAL NOT NULL, executed_qty REAL NOT NULL, average_fill_price REAL NOT NULL, "
            "rejection_reason TEXT NOT NULL, exchange_ts INTEGER NOT NULL, local_ts INTEGER NOT NULL);"
            "CREATE INDEX IF NOT EXISTS idx_order_events_client_ts ON order_events(client_order_id, local_ts);";
        if (sqlite3_exec(db_, ddl, nullptr, nullptr, nullptr) != SQLITE_OK) throw std::runtime_error("Failed to create order_events schema");
        const char* sql = "INSERT INTO order_events(client_order_id,exchange_order_id,symbol,side,state,source,requested_qty,executed_qty,average_fill_price,rejection_reason,exchange_ts,local_ts) VALUES(?,?,?,?,?,?,?,?,?,?,?,?);";
        if (sqlite3_prepare_v2(db_, sql, -1, &insert_, nullptr) != SQLITE_OK) throw std::runtime_error("Failed to prepare order-event insert");
    }

    ~OrderEventRepository() { if (insert_) sqlite3_finalize(insert_); if (db_) sqlite3_close(db_); }

    void save(const Snapshot& s, const std::string& source) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto ms = [](auto tp) { return std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count(); };
        sqlite3_reset(insert_); sqlite3_clear_bindings(insert_);
        sqlite3_bind_text(insert_,1,s.client_order_id.c_str(),-1,SQLITE_TRANSIENT);
        sqlite3_bind_int64(insert_,2,s.exchange_order_id);
        sqlite3_bind_text(insert_,3,s.symbol.c_str(),-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(insert_,4,s.side == Side::Buy ? "BUY" : "SELL",-1,SQLITE_STATIC);
        sqlite3_bind_text(insert_,5,to_string(s.state),-1,SQLITE_STATIC);
        sqlite3_bind_text(insert_,6,source.c_str(),-1,SQLITE_TRANSIENT);
        sqlite3_bind_double(insert_,7,s.requested_quantity);
        sqlite3_bind_double(insert_,8,s.executed_quantity);
        sqlite3_bind_double(insert_,9,s.average_fill_price);
        sqlite3_bind_text(insert_,10,s.rejection_reason.c_str(),-1,SQLITE_TRANSIENT);
        sqlite3_bind_int64(insert_,11,ms(s.updated_at));
        sqlite3_bind_int64(insert_,12,ms(std::chrono::system_clock::now()));
        if (sqlite3_step(insert_) != SQLITE_DONE) throw std::runtime_error("Failed to persist order event");
    }

private:
    sqlite3* db_ = nullptr;
    sqlite3_stmt* insert_ = nullptr;
    std::mutex mutex_;
};

} // namespace sentum::order
