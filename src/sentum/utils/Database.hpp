#pragma once

#include <string>
#include <utility>
#include <vector>
#include <sqlite3.h>

#include <sentum/api/model/Kline.hpp>

class Database {
public:
    explicit Database(const std::string& db_path);
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    bool save_klines(const std::string& symbol, const std::vector<Kline>& klines);
    bool save_kline_batch(const std::vector<std::pair<std::string, Kline>>& batch);
    std::vector<Kline> load_klines(const std::string& symbol, int limit = 100);

private:
    void exec_or_throw(const char* sql);
    bool ensure_table();
    bool bind_and_step(const std::string& symbol, const Kline& kline);

    sqlite3* db = nullptr;
    sqlite3_stmt* upsert_stmt = nullptr;
};
