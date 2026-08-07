#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <sqlite3.h>

namespace sentum::research {

struct ExperimentDatasetRecord {
    std::string dataset_id;
    std::string symbol;
    std::string source_path;
    std::string materialized_path;
    std::string sha256;
    std::int64_t from_ms = 0;
    std::int64_t to_ms = 0;
};

struct ExperimentManifest {
    std::string run_id;
    std::string name;
    std::string kind;
    std::string status = "started";
    std::int64_t started_at_ms = 0;
    std::int64_t finished_at_ms = 0;
    std::string git_commit;
    std::string config_sha256;
    std::string risk_sha256;
    std::string output_directory;
    std::vector<ExperimentDatasetRecord> datasets;
    std::vector<std::string> artifacts;
};

class Sha256 {
public:
    static std::string string(const std::string& value) {
        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        if (!ctx) throw std::runtime_error("EVP_MD_CTX_new failed");
        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned int length = 0;
        if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1 ||
            EVP_DigestUpdate(ctx, value.data(), value.size()) != 1 ||
            EVP_DigestFinal_ex(ctx, digest, &length) != 1) {
            EVP_MD_CTX_free(ctx);
            throw std::runtime_error("SHA256 calculation failed");
        }
        EVP_MD_CTX_free(ctx);
        std::ostringstream out;
        out << std::hex << std::setfill('0');
        for (unsigned int i = 0; i < length; ++i) out << std::setw(2) << static_cast<unsigned>(digest[i]);
        return out.str();
    }

    static std::string file(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) throw std::runtime_error("Cannot hash file: " + path);
        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        if (!ctx) throw std::runtime_error("EVP_MD_CTX_new failed");
        if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) {
            EVP_MD_CTX_free(ctx);
            throw std::runtime_error("SHA256 init failed");
        }
        char buffer[64 * 1024];
        while (in) {
            in.read(buffer, sizeof(buffer));
            const auto count = in.gcount();
            if (count > 0 && EVP_DigestUpdate(ctx, buffer, static_cast<std::size_t>(count)) != 1) {
                EVP_MD_CTX_free(ctx);
                throw std::runtime_error("SHA256 update failed");
            }
        }
        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned int length = 0;
        if (EVP_DigestFinal_ex(ctx, digest, &length) != 1) {
            EVP_MD_CTX_free(ctx);
            throw std::runtime_error("SHA256 final failed");
        }
        EVP_MD_CTX_free(ctx);
        std::ostringstream out;
        out << std::hex << std::setfill('0');
        for (unsigned int i = 0; i < length; ++i) out << std::setw(2) << static_cast<unsigned>(digest[i]);
        return out.str();
    }
};

inline std::int64_t unix_ms_now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

inline std::string make_run_id(const std::string& name, const std::string& config_hash,
                               const std::string& git_commit, std::int64_t started_at_ms) {
    const auto digest = Sha256::string(name + "|" + config_hash + "|" + git_commit + "|" + std::to_string(started_at_ms));
    return std::to_string(started_at_ms) + "-" + digest.substr(0, 12);
}

inline nlohmann::json manifest_json(const ExperimentManifest& m) {
    nlohmann::json datasets = nlohmann::json::array();
    for (const auto& d : m.datasets) datasets.push_back({
        {"dataset_id", d.dataset_id}, {"symbol", d.symbol}, {"source_path", d.source_path},
        {"materialized_path", d.materialized_path}, {"sha256", d.sha256},
        {"from_ms", d.from_ms}, {"to_ms", d.to_ms}
    });
    return {
        {"run_id",m.run_id},{"name",m.name},{"kind",m.kind},{"status",m.status},
        {"started_at_ms",m.started_at_ms},{"finished_at_ms",m.finished_at_ms},
        {"git_commit",m.git_commit},{"config_sha256",m.config_sha256},{"risk_sha256",m.risk_sha256},
        {"output_directory",m.output_directory},{"datasets",datasets},{"artifacts",m.artifacts}
    };
}

class ExperimentRepository {
public:
    explicit ExperimentRepository(const std::string& path = "log/experiments.sqlite3") {
        const auto parent = std::filesystem::path(path).parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent);
        if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) throw std::runtime_error("Cannot open experiment registry");
        exec("PRAGMA journal_mode=WAL;");
        exec("PRAGMA synchronous=NORMAL;");
        exec("CREATE TABLE IF NOT EXISTS research_runs("
             "run_id TEXT PRIMARY KEY,name TEXT NOT NULL,kind TEXT NOT NULL,status TEXT NOT NULL,"
             "started_at_ms INTEGER NOT NULL,finished_at_ms INTEGER NOT NULL DEFAULT 0,"
             "git_commit TEXT,config_sha256 TEXT,risk_sha256 TEXT,output_directory TEXT);");
        exec("CREATE TABLE IF NOT EXISTS research_datasets("
             "run_id TEXT NOT NULL,dataset_id TEXT NOT NULL,symbol TEXT NOT NULL,source_path TEXT NOT NULL,"
             "materialized_path TEXT NOT NULL,sha256 TEXT NOT NULL,from_ms INTEGER,to_ms INTEGER,"
             "PRIMARY KEY(run_id,dataset_id));");
        exec("CREATE TABLE IF NOT EXISTS research_artifacts("
             "run_id TEXT NOT NULL,path TEXT NOT NULL,sha256 TEXT NOT NULL,PRIMARY KEY(run_id,path));");
    }

    ~ExperimentRepository() { if (db_) sqlite3_close(db_); }
    ExperimentRepository(const ExperimentRepository&) = delete;
    ExperimentRepository& operator=(const ExperimentRepository&) = delete;

    void save(const ExperimentManifest& m) {
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "INSERT OR REPLACE INTO research_runs(run_id,name,kind,status,started_at_ms,finished_at_ms,git_commit,config_sha256,risk_sha256,output_directory) VALUES(?,?,?,?,?,?,?,?,?,?);";
        prepare(sql, &stmt);
        bind(stmt, 1, m.run_id); bind(stmt, 2, m.name); bind(stmt, 3, m.kind); bind(stmt, 4, m.status);
        sqlite3_bind_int64(stmt,5,m.started_at_ms); sqlite3_bind_int64(stmt,6,m.finished_at_ms);
        bind(stmt,7,m.git_commit); bind(stmt,8,m.config_sha256); bind(stmt,9,m.risk_sha256); bind(stmt,10,m.output_directory);
        step_finalize(stmt);

        for (const auto& d : m.datasets) {
            prepare("INSERT OR REPLACE INTO research_datasets(run_id,dataset_id,symbol,source_path,materialized_path,sha256,from_ms,to_ms) VALUES(?,?,?,?,?,?,?,?);", &stmt);
            bind(stmt,1,m.run_id); bind(stmt,2,d.dataset_id); bind(stmt,3,d.symbol); bind(stmt,4,d.source_path);
            bind(stmt,5,d.materialized_path); bind(stmt,6,d.sha256); sqlite3_bind_int64(stmt,7,d.from_ms); sqlite3_bind_int64(stmt,8,d.to_ms);
            step_finalize(stmt);
        }
        for (const auto& artifact : m.artifacts) {
            if (!std::filesystem::exists(artifact)) continue;
            prepare("INSERT OR REPLACE INTO research_artifacts(run_id,path,sha256) VALUES(?,?,?);", &stmt);
            bind(stmt,1,m.run_id); bind(stmt,2,artifact); bind(stmt,3,Sha256::file(artifact));
            step_finalize(stmt);
        }
    }

private:
    sqlite3* db_ = nullptr;
    void exec(const char* sql) {
        char* error = nullptr;
        if (sqlite3_exec(db_, sql, nullptr, nullptr, &error) != SQLITE_OK) {
            const std::string message = error ? error : "sqlite error";
            sqlite3_free(error);
            throw std::runtime_error(message);
        }
    }
    void prepare(const char* sql, sqlite3_stmt** stmt) {
        if (sqlite3_prepare_v2(db_, sql, -1, stmt, nullptr) != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db_));
    }
    static void bind(sqlite3_stmt* stmt, int index, const std::string& value) {
        sqlite3_bind_text(stmt,index,value.c_str(),-1,SQLITE_TRANSIENT);
    }
    void step_finalize(sqlite3_stmt* stmt) {
        if (sqlite3_step(stmt) != SQLITE_DONE) { const std::string e = sqlite3_errmsg(db_); sqlite3_finalize(stmt); throw std::runtime_error(e); }
        sqlite3_finalize(stmt);
    }
};

inline void write_manifest(const ExperimentManifest& manifest) {
    std::filesystem::create_directories(manifest.output_directory);
    const auto path = std::filesystem::path(manifest.output_directory) / "manifest.json";
    const auto tmp = path.string() + ".tmp";
    { std::ofstream out(tmp, std::ios::trunc); if (!out) throw std::runtime_error("Cannot write experiment manifest"); out << manifest_json(manifest).dump(2) << '\n'; }
    std::error_code ec; std::filesystem::remove(path, ec); ec.clear(); std::filesystem::rename(tmp, path, ec);
    if (ec) throw std::runtime_error("Cannot publish experiment manifest: " + ec.message());
}

} // namespace sentum::research
