#include <sentum/dashboard/DashboardRepository.hpp>
#include <sqlite3.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>

int main() {
    const auto path = std::filesystem::temp_directory_path() / "sentum_equity_history_regression.sqlite3";
    std::filesystem::remove(path);
    sqlite3* db = nullptr;
    if (sqlite3_open(path.string().c_str(), &db) != SQLITE_OK) return 1;
    char* error = nullptr;
    const char* sql = "CREATE TABLE trades (id INTEGER PRIMARY KEY, exit_ts INTEGER, net_profit REAL);"
                      "INSERT INTO trades(exit_ts,net_profit) VALUES (10,100),(20,-20),(30,5),(30,7),(40,-2);";
    const int status = sqlite3_exec(db, sql, nullptr, nullptr, &error);
    if (error) sqlite3_free(error);
    sqlite3_close(db);
    if (status != SQLITE_OK) return 1;
    const sentum::dashboard::DashboardRepository repository(path.string());
    const auto rows = repository.equity_curve(3);
    const auto most_recent = repository.equity_curve(1);
    const auto entire_history = repository.equity_curve(100);
    std::filesystem::remove(path);
    if (most_recent.size() != 1 || most_recent[0]["ts"] != 40 ||
        most_recent[0]["equity"] != 90.0 || entire_history.size() != 5 ||
        entire_history[0]["equity"] != 100.0) return 2;
    if (rows.size() != 3 || rows[0]["ts"] != 30 || rows[0]["equity"] != 85.0 ||
        rows[1]["equity"] != 92.0 || rows[2]["ts"] != 40 || rows[2]["equity"] != 90.0) {
        std::cerr << "Recent realized equity and cumulative balance mismatch\n";
        return 1;
    }
    return 0;
}
