#pragma once

#include <chrono>
#include <stdexcept>
#include <string>

#include <sqlite3.h>

namespace sentum::operations {

class OperationalEventRepository {
public:
    explicit OperationalEventRepository(const std::string& path) {
        if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) throw std::runtime_error("Failed to open operational event database");
        const char* ddl =
            "CREATE TABLE IF NOT EXISTS runtime_events(id INTEGER PRIMARY KEY AUTOINCREMENT,ts_ms INTEGER NOT NULL,type TEXT NOT NULL,severity TEXT NOT NULL,details TEXT NOT NULL);"
            "CREATE TABLE IF NOT EXISTS reconciliation_runs(id INTEGER PRIMARY KEY AUTOINCREMENT,started_ms INTEGER NOT NULL,finished_ms INTEGER NOT NULL,success INTEGER NOT NULL,open_orders INTEGER NOT NULL,balances_checked INTEGER NOT NULL,inconsistencies INTEGER NOT NULL,details TEXT NOT NULL);"
            "CREATE TABLE IF NOT EXISTS kill_switch_events(id INTEGER PRIMARY KEY AUTOINCREMENT,ts_ms INTEGER NOT NULL,active INTEGER NOT NULL,reason TEXT NOT NULL);";
        if (sqlite3_exec(db_, ddl, nullptr, nullptr, nullptr) != SQLITE_OK) throw std::runtime_error("Failed to create operational tables");
    }
    ~OperationalEventRepository() { if (db_) sqlite3_close(db_); }

    void runtime(const std::string& type, const std::string& severity, const std::string& details) { exec("INSERT INTO runtime_events(ts_ms,type,severity,details) VALUES(?,?,?,?)", type, severity, details); }
    void kill_switch(bool active, const std::string& reason) { exec_kill(active, reason); }
    void reconciliation(std::int64_t started, std::int64_t finished, bool success, int orders, int balances, int inconsistencies, const std::string& details) {
        sqlite3_stmt* s=nullptr; const char* sql="INSERT INTO reconciliation_runs(started_ms,finished_ms,success,open_orders,balances_checked,inconsistencies,details) VALUES(?,?,?,?,?,?,?)";
        if (sqlite3_prepare_v2(db_,sql,-1,&s,nullptr)!=SQLITE_OK) throw std::runtime_error("prepare reconciliation failed");
        sqlite3_bind_int64(s,1,started); sqlite3_bind_int64(s,2,finished); sqlite3_bind_int(s,3,success); sqlite3_bind_int(s,4,orders); sqlite3_bind_int(s,5,balances); sqlite3_bind_int(s,6,inconsistencies); sqlite3_bind_text(s,7,details.c_str(),-1,SQLITE_TRANSIENT);
        if (sqlite3_step(s)!=SQLITE_DONE) { sqlite3_finalize(s); throw std::runtime_error("persist reconciliation failed"); } sqlite3_finalize(s);
    }
private:
    static std::int64_t now_ms(){return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();}
    void exec(const char* sql,const std::string&a,const std::string&b,const std::string&c){sqlite3_stmt*s=nullptr;sqlite3_prepare_v2(db_,sql,-1,&s,nullptr);sqlite3_bind_int64(s,1,now_ms());sqlite3_bind_text(s,2,a.c_str(),-1,SQLITE_TRANSIENT);sqlite3_bind_text(s,3,b.c_str(),-1,SQLITE_TRANSIENT);sqlite3_bind_text(s,4,c.c_str(),-1,SQLITE_TRANSIENT);if(sqlite3_step(s)!=SQLITE_DONE){sqlite3_finalize(s);throw std::runtime_error("persist runtime event failed");}sqlite3_finalize(s);}
    void exec_kill(bool active,const std::string&reason){sqlite3_stmt*s=nullptr;sqlite3_prepare_v2(db_,"INSERT INTO kill_switch_events(ts_ms,active,reason) VALUES(?,?,?)",-1,&s,nullptr);sqlite3_bind_int64(s,1,now_ms());sqlite3_bind_int(s,2,active);sqlite3_bind_text(s,3,reason.c_str(),-1,SQLITE_TRANSIENT);if(sqlite3_step(s)!=SQLITE_DONE){sqlite3_finalize(s);throw std::runtime_error("persist kill switch failed");}sqlite3_finalize(s);}
    sqlite3* db_=nullptr;
};

} // namespace sentum::operations
