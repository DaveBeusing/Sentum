#pragma once

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <sqlite3.h>

namespace sentum::dashboard {

class DashboardRepository {
public:
    explicit DashboardRepository(std::string db_path = "log/klines.sqlite3") : db_path_(std::move(db_path)) {}

    nlohmann::json recent_trades(int limit = 100) const {
        return query("SELECT symbol,strategy,signal_reason,entry_ts,exit_ts,entry_price,exit_price,quantity,gross_profit,fees,net_profit,exit_reason,simulated FROM trades ORDER BY id DESC LIMIT ?;",
            limit,
            {"symbol","strategy","signal_reason","entry_ts","exit_ts","entry_price","exit_price","quantity","gross_profit","fees","net_profit","exit_reason","simulated"});
    }

    nlohmann::json recent_orders(int limit = 100) const {
        return query("SELECT client_order_id,exchange_order_id,symbol,side,state,source,requested_qty,executed_qty,average_fill_price,rejection_reason,exchange_ts,local_ts FROM order_events ORDER BY id DESC LIMIT ?;",
            limit,
            {"client_order_id","exchange_order_id","symbol","side","state","source","requested_quantity","executed_quantity","average_fill_price","rejection_reason","exchange_ts","local_ts"});
    }

    nlohmann::json equity_curve(int limit = 500) const {
        sqlite3* db = open();
        nlohmann::json result = nlohmann::json::array();
        if (!db) return result;
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "SELECT exit_ts,net_profit FROM trades ORDER BY exit_ts ASC LIMIT ?;";
        double equity = 0.0;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, std::clamp(limit, 1, 5000));
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                equity += sqlite3_column_double(stmt, 1);
                result.push_back({{"ts", sqlite3_column_int64(stmt, 0)}, {"equity", equity}});
            }
        }
        if (stmt) sqlite3_finalize(stmt);
        sqlite3_close(db);
        return result;
    }

    nlohmann::json replay_metrics() const {
        std::ifstream file("log/replay_metrics.json");
        if (!file) return nlohmann::json::object();
        try { nlohmann::json value; file >> value; return value; }
        catch (...) { return nlohmann::json::object(); }
    }

private:
    sqlite3* open() const {
        sqlite3* db = nullptr;
        if (sqlite3_open_v2(db_path_.c_str(), &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
            if (db) sqlite3_close(db);
            return nullptr;
        }
        sqlite3_busy_timeout(db, 1000);
        return db;
    }

    nlohmann::json query(const char* sql, int limit, const std::vector<std::string>& columns) const {
        sqlite3* db = open();
        if (!db) return nlohmann::json::array();
        sqlite3_stmt* stmt = nullptr;
        nlohmann::json result = nlohmann::json::array();
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, std::clamp(limit, 1, 1000));
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                nlohmann::json row = nlohmann::json::object();
                const int count = sqlite3_column_count(stmt);
                for (int i = 0; i < count && i < static_cast<int>(columns.size()); ++i) {
                    switch (sqlite3_column_type(stmt, i)) {
                        case SQLITE_INTEGER: row[columns[i]] = sqlite3_column_int64(stmt, i); break;
                        case SQLITE_FLOAT: row[columns[i]] = sqlite3_column_double(stmt, i); break;
                        case SQLITE_TEXT: row[columns[i]] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, i)); break;
                        case SQLITE_NULL: row[columns[i]] = nullptr; break;
                        default: row[columns[i]] = nullptr; break;
                    }
                }
                result.push_back(std::move(row));
            }
        }
        if (stmt) sqlite3_finalize(stmt);
        sqlite3_close(db);
        return result;
    }

    std::string db_path_;
};

} // namespace sentum::dashboard
