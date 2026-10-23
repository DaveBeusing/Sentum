#pragma once

#include <string>
#include <utility>
#include <vector>
#include <sqlite3.h>

#include <sentum/api/model/Kline.hpp>

struct KlineBatchItem {
    const std::string* symbol = nullptr;
    Kline kline;
};

struct KlineWriteResult {
    bool committed = false;
    int sqlite_code = SQLITE_OK;
    int rollback_code = SQLITE_OK;

    bool retryable() const noexcept {
        const int primary = sqlite_code & 0xff;
        return !committed && rollback_code == SQLITE_OK &&
               (primary == SQLITE_BUSY || primary == SQLITE_LOCKED);
    }
};

class Database {
public:
    explicit Database(const std::string& db_path);
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    bool save_klines(const std::string& symbol, const std::vector<Kline>& klines);
    bool save_kline_batch(const std::vector<std::pair<std::string, Kline>>& batch);
    bool save_kline_batch(const std::vector<KlineBatchItem>& batch);
    KlineWriteResult save_kline_batch_result(const std::vector<KlineBatchItem>& batch,
                                            int busy_timeout_ms = 5000);
    std::vector<Kline> load_klines(const std::string& symbol, int limit = 100);

private:
    friend struct DatabaseTestAccess;
    void exec_or_throw(const char* sql);
    bool ensure_table();
    int bind_and_step(const std::string& symbol, const Kline& kline);
    KlineWriteResult failed_write(int sqlite_code);

    sqlite3* db = nullptr;
    sqlite3_stmt* upsert_stmt = nullptr;
};
