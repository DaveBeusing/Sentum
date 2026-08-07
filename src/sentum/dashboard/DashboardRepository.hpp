#pragma once

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <sqlite3.h>

namespace sentum::dashboard {

class DashboardRepository {
public:
    explicit DashboardRepository(std::string db_path = "log/klines.sqlite3",
                                 std::string experiment_db_path = "log/experiments.sqlite3")
        : db_path_(std::move(db_path)), experiment_db_path_(std::move(experiment_db_path)) {}

    nlohmann::json recent_trades(int limit = 100) const {
        return query(db_path_, "SELECT symbol,strategy,signal_reason,entry_ts,exit_ts,entry_price,exit_price,quantity,gross_profit,fees,net_profit,exit_reason,simulated FROM trades ORDER BY id DESC LIMIT ?;",
            limit, {"symbol","strategy","signal_reason","entry_ts","exit_ts","entry_price","exit_price","quantity","gross_profit","fees","net_profit","exit_reason","simulated"});
    }

    nlohmann::json recent_orders(int limit = 100) const {
        return query(db_path_, "SELECT client_order_id,exchange_order_id,symbol,side,state,source,requested_qty,executed_qty,average_fill_price,rejection_reason,exchange_ts,local_ts FROM order_events ORDER BY id DESC LIMIT ?;",
            limit, {"client_order_id","exchange_order_id","symbol","side","state","source","requested_quantity","executed_quantity","average_fill_price","rejection_reason","exchange_ts","local_ts"});
    }

    nlohmann::json equity_curve(int limit = 500) const {
        sqlite3* db = open_readonly(db_path_);
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

    nlohmann::json replay_metrics() const { return read_json("log/replay_metrics.json"); }
    nlohmann::json research_results() const { return read_json("log/research_latest.json"); }

    nlohmann::json experiment_runs(int limit = 100) const {
        return query(experiment_db_path_,
            "SELECT run_id,name,kind,status,started_at_ms,finished_at_ms,git_commit,config_sha256,risk_sha256,output_directory FROM research_runs ORDER BY started_at_ms DESC LIMIT ?;",
            limit, {"run_id","name","kind","status","started_at_ms","finished_at_ms","git_commit","config_sha256","risk_sha256","output_directory"});
    }

    nlohmann::json experiment_detail(const std::string& run_id) const {
        sqlite3* db = open_readonly(experiment_db_path_);
        if (!db) return nlohmann::json::object();
        nlohmann::json out = nlohmann::json::object();
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "SELECT run_id,name,kind,status,started_at_ms,finished_at_ms,git_commit,config_sha256,risk_sha256,output_directory FROM research_runs WHERE run_id=? LIMIT 1;";
        std::string root;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, run_id.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                out = row_json(stmt, {"run_id","name","kind","status","started_at_ms","finished_at_ms","git_commit","config_sha256","risk_sha256","output_directory"});
                if (!out["output_directory"].is_null()) root = out["output_directory"].get<std::string>();
            }
        }
        if (stmt) sqlite3_finalize(stmt);
        if (out.empty()) { sqlite3_close(db); return out; }

        out["datasets"] = nlohmann::json::array();
        if (sqlite3_prepare_v2(db, "SELECT dataset_id,symbol,source_path,materialized_path,sha256,from_ms,to_ms FROM research_datasets WHERE run_id=? ORDER BY dataset_id;", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, run_id.c_str(), -1, SQLITE_TRANSIENT);
            while (sqlite3_step(stmt) == SQLITE_ROW)
                out["datasets"].push_back(row_json(stmt, {"dataset_id","symbol","source_path","materialized_path","sha256","from_ms","to_ms"}));
        }
        if (stmt) sqlite3_finalize(stmt);

        out["artifacts"] = nlohmann::json::array();
        if (sqlite3_prepare_v2(db, "SELECT path,sha256 FROM research_artifacts WHERE run_id=? ORDER BY path;", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, run_id.c_str(), -1, SQLITE_TRANSIENT);
            while (sqlite3_step(stmt) == SQLITE_ROW)
                out["artifacts"].push_back(row_json(stmt, {"path","sha256"}));
        }
        if (stmt) sqlite3_finalize(stmt);
        sqlite3_close(db);

        if (!root.empty()) {
            out["research"] = read_json(root + "/research.json");
            out["visualization"] = read_json(root + "/research-visualization.json");
            out["portfolio"] = read_json(root + "/portfolio-research.json");
        }
        return out;
    }

    nlohmann::json experiment_trials(const std::string& run_id, int limit = 5000) const {
        const auto detail = experiment_detail(run_id);
        if (!detail.is_object() || !detail.contains("output_directory") || detail["output_directory"].is_null())
            return nlohmann::json::array();
        std::ifstream file(detail["output_directory"].get<std::string>() + "/trials.csv");
        if (!file) return nlohmann::json::array();
        std::string header;
        if (!std::getline(file, header)) return nlohmann::json::array();
        const auto columns = split_csv(header);
        nlohmann::json rows = nlohmann::json::array();
        std::string line;
        const int maximum = std::clamp(limit, 1, 10000);
        while (static_cast<int>(rows.size()) < maximum && std::getline(file, line)) {
            const auto values = split_csv(line);
            nlohmann::json row = nlohmann::json::object();
            for (std::size_t i = 0; i < columns.size() && i < values.size(); ++i)
                row[columns[i]] = parse_scalar(values[i]);
            rows.push_back(std::move(row));
        }
        return rows;
    }

private:
    static nlohmann::json read_json(const std::string& path) {
        std::ifstream file(path);
        if (!file) return nlohmann::json::object();
        try { nlohmann::json value; file >> value; return value; }
        catch (...) { return nlohmann::json::object(); }
    }

    static sqlite3* open_readonly(const std::string& path) {
        sqlite3* db = nullptr;
        if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
            if (db) sqlite3_close(db);
            return nullptr;
        }
        sqlite3_busy_timeout(db, 1000);
        return db;
    }

    static nlohmann::json row_json(sqlite3_stmt* stmt, const std::vector<std::string>& columns) {
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
        return row;
    }

    static nlohmann::json query(const std::string& path, const char* sql, int limit,
                                const std::vector<std::string>& columns) {
        sqlite3* db = open_readonly(path);
        if (!db) return nlohmann::json::array();
        sqlite3_stmt* stmt = nullptr;
        nlohmann::json result = nlohmann::json::array();
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, std::clamp(limit, 1, 10000));
            while (sqlite3_step(stmt) == SQLITE_ROW) result.push_back(row_json(stmt, columns));
        }
        if (stmt) sqlite3_finalize(stmt);
        sqlite3_close(db);
        return result;
    }

    static std::vector<std::string> split_csv(const std::string& line) {
        std::vector<std::string> out;
        std::stringstream stream(line);
        std::string cell;
        while (std::getline(stream, cell, ',')) out.push_back(cell);
        return out;
    }

    static nlohmann::json parse_scalar(const std::string& value) {
        if (value == "true") return true;
        if (value == "false") return false;
        try {
            std::size_t used = 0;
            const double number = std::stod(value, &used);
            if (used == value.size()) return number;
        } catch (...) {}
        return value;
    }

    std::string db_path_;
    std::string experiment_db_path_;
};

} // namespace sentum::dashboard
