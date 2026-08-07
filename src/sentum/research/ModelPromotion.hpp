#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <sentum/api/BinanceWebsocketClient.hpp>
#include <sentum/backtest/Backtest.hpp>
#include <sentum/research/ExperimentManager.hpp>
#include <sentum/time/Clock.hpp>
#include <sentum/trader/TradeEngine.hpp>
#include <sentum/trader/strategy/StrategyFramework.hpp>
#include <sentum/trader/types/RiskConfig.hpp>

namespace sentum::promotion {

enum class ModelStage { Research, Shadow, Paper, Testnet };

inline std::string stage_name(ModelStage stage) {
    switch (stage) {
        case ModelStage::Research: return "research";
        case ModelStage::Shadow: return "shadow";
        case ModelStage::Paper: return "paper";
        case ModelStage::Testnet: return "testnet";
    }
    return "unknown";
}

inline ModelStage parse_stage(const std::string& value) {
    if (value == "research") return ModelStage::Research;
    if (value == "shadow") return ModelStage::Shadow;
    if (value == "paper") return ModelStage::Paper;
    if (value == "testnet") return ModelStage::Testnet;
    throw std::runtime_error("Unsupported model stage: " + value);
}

inline bool valid_transition(ModelStage from, ModelStage to) {
    return (from == ModelStage::Research && to == ModelStage::Shadow) ||
           (from == ModelStage::Shadow && to == ModelStage::Paper) ||
           (from == ModelStage::Paper && to == ModelStage::Testnet);
}

struct PromotionPolicy {
    std::size_t min_trades = 30;
    double min_net_profit = 0.0;
    double min_profit_factor = 1.10;
    double min_win_rate = 45.0;
    double min_sharpe = 0.50;
    double max_drawdown = 0.0; // 0 disables absolute cap
};

struct StageEvidence {
    std::string stage;
    std::int64_t started_at_ms = 0;
    std::int64_t finished_at_ms = 0;
    std::size_t trades = 0;
    double net_profit = 0.0;
    double max_drawdown = 0.0;
    double profit_factor = 0.0;
    double win_rate = 0.0;
    double expectancy = 0.0;
    double sharpe = 0.0;
    double sortino = 0.0;
    std::string artifact;
};

struct ModelDefinition {
    std::string model_id;
    std::string name;
    std::string symbol;
    std::string source_experiment_run;
    std::string source_git_commit;
    std::string source_config_sha256;
    std::string risk_config = "config/risk.json";
    nlohmann::json strategy;
    PromotionPolicy policy;
};

inline ModelDefinition load_model_definition(const std::string& path) {
    std::ifstream file(path);
    if (!file) throw std::runtime_error("Cannot open model definition: " + path);
    nlohmann::json j; file >> j;
    ModelDefinition d;
    d.model_id = j.value("model_id", std::string{});
    d.name = j.value("name", d.model_id);
    d.symbol = j.value("symbol", std::string{});
    d.source_experiment_run = j.value("source_experiment_run", std::string{});
    d.source_git_commit = j.value("source_git_commit", std::string{});
    d.source_config_sha256 = j.value("source_config_sha256", std::string{});
    d.risk_config = j.value("risk_config", d.risk_config);
    d.strategy = j.value("strategy", nlohmann::json::object());
    if (j.contains("promotion_policy")) {
        const auto& p = j.at("promotion_policy");
        d.policy.min_trades = p.value("min_trades", d.policy.min_trades);
        d.policy.min_net_profit = p.value("min_net_profit", d.policy.min_net_profit);
        d.policy.min_profit_factor = p.value("min_profit_factor", d.policy.min_profit_factor);
        d.policy.min_win_rate = p.value("min_win_rate", d.policy.min_win_rate);
        d.policy.min_sharpe = p.value("min_sharpe", d.policy.min_sharpe);
        d.policy.max_drawdown = p.value("max_drawdown", d.policy.max_drawdown);
    }
    if (d.model_id.empty() || d.symbol.empty() || d.source_experiment_run.empty() || !d.strategy.is_object())
        throw std::runtime_error("Model definition requires model_id, symbol, source_experiment_run and strategy");
    return d;
}

inline StageEvidence evidence_from_metrics(const std::string& stage, const BacktestMetrics& m,
                                           std::int64_t start, std::int64_t finish, std::string artifact = {}) {
    return {stage,start,finish,m.trades,m.net_profit,m.max_drawdown,m.profit_factor,m.win_rate,m.expectancy,m.sharpe,m.sortino,std::move(artifact)};
}

inline bool passes_policy(const StageEvidence& e, const PromotionPolicy& p, std::string& reason) {
    if (e.trades < p.min_trades) { reason = "insufficient trades"; return false; }
    if (e.net_profit < p.min_net_profit) { reason = "net profit below threshold"; return false; }
    if (!std::isfinite(e.profit_factor) || e.profit_factor < p.min_profit_factor) { reason = "profit factor below threshold"; return false; }
    if (e.win_rate < p.min_win_rate) { reason = "win rate below threshold"; return false; }
    if (e.sharpe < p.min_sharpe) { reason = "Sharpe below threshold"; return false; }
    if (p.max_drawdown > 0.0 && e.max_drawdown > p.max_drawdown) { reason = "drawdown above threshold"; return false; }
    reason = "promotion gates passed";
    return true;
}

class ModelRegistry {
public:
    explicit ModelRegistry(std::string path = "log/models.sqlite3") : path_(std::move(path)) {
        const auto parent = std::filesystem::path(path_).parent_path(); if (!parent.empty()) std::filesystem::create_directories(parent);
        if (sqlite3_open(path_.c_str(), &db_) != SQLITE_OK) throw std::runtime_error("Cannot open model registry");
        exec("PRAGMA journal_mode=WAL;"); exec("PRAGMA synchronous=NORMAL;");
        exec("CREATE TABLE IF NOT EXISTS models(model_id TEXT PRIMARY KEY,name TEXT,symbol TEXT,stage TEXT NOT NULL,source_experiment_run TEXT NOT NULL,source_git_commit TEXT,source_config_sha256 TEXT,definition_json TEXT NOT NULL,created_at_ms INTEGER NOT NULL,updated_at_ms INTEGER NOT NULL);");
        exec("CREATE TABLE IF NOT EXISTS stage_evidence(id INTEGER PRIMARY KEY AUTOINCREMENT,model_id TEXT NOT NULL,stage TEXT NOT NULL,started_at_ms INTEGER,finished_at_ms INTEGER,trades INTEGER,net_profit REAL,max_drawdown REAL,profit_factor REAL,win_rate REAL,expectancy REAL,sharpe REAL,sortino REAL,artifact TEXT);");
        exec("CREATE TABLE IF NOT EXISTS promotion_events(id INTEGER PRIMARY KEY AUTOINCREMENT,model_id TEXT NOT NULL,from_stage TEXT NOT NULL,to_stage TEXT NOT NULL,approved INTEGER NOT NULL,reason TEXT NOT NULL,operator_confirmation TEXT,created_at_ms INTEGER NOT NULL);");
    }
    ~ModelRegistry(){ if(db_) sqlite3_close(db_); }
    ModelRegistry(const ModelRegistry&) = delete; ModelRegistry& operator=(const ModelRegistry&) = delete;

    void register_model(const ModelDefinition& d, const std::string& definition_json) {
        const auto now = sentum::research::unix_ms_now(); sqlite3_stmt* s=nullptr;
        prepare("INSERT INTO models(model_id,name,symbol,stage,source_experiment_run,source_git_commit,source_config_sha256,definition_json,created_at_ms,updated_at_ms) VALUES(?,?,?,?,?,?,?,?,?,?) ON CONFLICT(model_id) DO UPDATE SET name=excluded.name,symbol=excluded.symbol,source_experiment_run=excluded.source_experiment_run,source_git_commit=excluded.source_git_commit,source_config_sha256=excluded.source_config_sha256,definition_json=excluded.definition_json,updated_at_ms=excluded.updated_at_ms;",&s);
        bind(s,1,d.model_id);bind(s,2,d.name);bind(s,3,d.symbol);bind(s,4,"research");bind(s,5,d.source_experiment_run);bind(s,6,d.source_git_commit);bind(s,7,d.source_config_sha256);bind(s,8,definition_json);sqlite3_bind_int64(s,9,now);sqlite3_bind_int64(s,10,now);done(s);
    }

    std::string stage(const std::string& model_id) const {
        sqlite3_stmt* s=nullptr; prepare_const("SELECT stage FROM models WHERE model_id=?;",&s); bind(s,1,model_id); std::string out;
        if(sqlite3_step(s)==SQLITE_ROW) out=reinterpret_cast<const char*>(sqlite3_column_text(s,0)); sqlite3_finalize(s);
        if(out.empty()) throw std::runtime_error("Unknown model: "+model_id); return out;
    }

    void save_evidence(const std::string& model_id, const StageEvidence& e) {
        sqlite3_stmt* s=nullptr; prepare("INSERT INTO stage_evidence(model_id,stage,started_at_ms,finished_at_ms,trades,net_profit,max_drawdown,profit_factor,win_rate,expectancy,sharpe,sortino,artifact) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?);",&s);
        bind(s,1,model_id);bind(s,2,e.stage);sqlite3_bind_int64(s,3,e.started_at_ms);sqlite3_bind_int64(s,4,e.finished_at_ms);sqlite3_bind_int64(s,5,static_cast<sqlite3_int64>(e.trades));sqlite3_bind_double(s,6,e.net_profit);sqlite3_bind_double(s,7,e.max_drawdown);sqlite3_bind_double(s,8,e.profit_factor);sqlite3_bind_double(s,9,e.win_rate);sqlite3_bind_double(s,10,e.expectancy);sqlite3_bind_double(s,11,e.sharpe);sqlite3_bind_double(s,12,e.sortino);bind(s,13,e.artifact);done(s);
    }

    StageEvidence latest_evidence(const std::string& model_id, const std::string& stage_value) const {
        sqlite3_stmt* s=nullptr; prepare_const("SELECT stage,started_at_ms,finished_at_ms,trades,net_profit,max_drawdown,profit_factor,win_rate,expectancy,sharpe,sortino,artifact FROM stage_evidence WHERE model_id=? AND stage=? ORDER BY id DESC LIMIT 1;",&s);bind(s,1,model_id);bind(s,2,stage_value);
        StageEvidence e; if(sqlite3_step(s)!=SQLITE_ROW){sqlite3_finalize(s);throw std::runtime_error("No stage evidence for "+stage_value);}
        e.stage=text(s,0);e.started_at_ms=sqlite3_column_int64(s,1);e.finished_at_ms=sqlite3_column_int64(s,2);e.trades=static_cast<std::size_t>(sqlite3_column_int64(s,3));e.net_profit=sqlite3_column_double(s,4);e.max_drawdown=sqlite3_column_double(s,5);e.profit_factor=sqlite3_column_double(s,6);e.win_rate=sqlite3_column_double(s,7);e.expectancy=sqlite3_column_double(s,8);e.sharpe=sqlite3_column_double(s,9);e.sortino=sqlite3_column_double(s,10);e.artifact=text(s,11);sqlite3_finalize(s);return e;
    }

    void promote(const ModelDefinition& d, ModelStage target, const std::string& confirmation) {
        if (target == ModelStage::Research) throw std::runtime_error("Cannot promote to research");
        const auto current = parse_stage(stage(d.model_id));
        if (!valid_transition(current,target)) throw std::runtime_error("Invalid promotion transition: "+stage_name(current)+" -> "+stage_name(target));
        if (confirmation != "I_APPROVE_MODEL_PROMOTION") throw std::runtime_error("Explicit operator confirmation required");
        std::string reason;
        const auto evidence = latest_evidence(d.model_id, stage_name(current));
        const bool approved = passes_policy(evidence,d.policy,reason);
        record_event(d.model_id,current,target,approved,reason,confirmation);
        if(!approved) throw std::runtime_error("Promotion blocked: "+reason);
        sqlite3_stmt* s=nullptr;prepare("UPDATE models SET stage=?,updated_at_ms=? WHERE model_id=?;",&s);bind(s,1,stage_name(target));sqlite3_bind_int64(s,2,sentum::research::unix_ms_now());bind(s,3,d.model_id);done(s);
    }

    nlohmann::json snapshot(const std::string& model_id) const {
        sqlite3_stmt* s=nullptr;prepare_const("SELECT model_id,name,symbol,stage,source_experiment_run,source_git_commit,source_config_sha256,created_at_ms,updated_at_ms FROM models WHERE model_id=?;",&s);bind(s,1,model_id);if(sqlite3_step(s)!=SQLITE_ROW){sqlite3_finalize(s);return nlohmann::json::object();}
        nlohmann::json j{{"model_id",text(s,0)},{"name",text(s,1)},{"symbol",text(s,2)},{"stage",text(s,3)},{"source_experiment_run",text(s,4)},{"source_git_commit",text(s,5)},{"source_config_sha256",text(s,6)},{"created_at_ms",sqlite3_column_int64(s,7)},{"updated_at_ms",sqlite3_column_int64(s,8)}};sqlite3_finalize(s);return j;
    }

private:
    std::string path_; sqlite3* db_=nullptr;
    static std::string text(sqlite3_stmt*s,int i){const auto*p=sqlite3_column_text(s,i);return p?reinterpret_cast<const char*>(p):"";}
    static void bind(sqlite3_stmt*s,int i,const std::string&v){sqlite3_bind_text(s,i,v.c_str(),-1,SQLITE_TRANSIENT);}
    void exec(const char*sql){char*e=nullptr;if(sqlite3_exec(db_,sql,nullptr,nullptr,&e)!=SQLITE_OK){std::string m=e?e:"sqlite";sqlite3_free(e);throw std::runtime_error(m);}}
    void prepare(const char*sql,sqlite3_stmt**s){if(sqlite3_prepare_v2(db_,sql,-1,s,nullptr)!=SQLITE_OK)throw std::runtime_error(sqlite3_errmsg(db_));}
    void prepare_const(const char*sql,sqlite3_stmt**s)const{if(sqlite3_prepare_v2(db_,sql,-1,s,nullptr)!=SQLITE_OK)throw std::runtime_error(sqlite3_errmsg(db_));}
    void done(sqlite3_stmt*s){if(sqlite3_step(s)!=SQLITE_DONE){auto e=std::string(sqlite3_errmsg(db_));sqlite3_finalize(s);throw std::runtime_error(e);}sqlite3_finalize(s);}
    void record_event(const std::string&id,ModelStage from,ModelStage to,bool approved,const std::string&reason,const std::string&confirmation){sqlite3_stmt*s=nullptr;prepare("INSERT INTO promotion_events(model_id,from_stage,to_stage,approved,reason,operator_confirmation,created_at_ms) VALUES(?,?,?,?,?,?,?);",&s);bind(s,1,id);bind(s,2,stage_name(from));bind(s,3,stage_name(to));sqlite3_bind_int(s,4,approved?1:0);bind(s,5,reason);bind(s,6,confirmation);sqlite3_bind_int64(s,7,sentum::research::unix_ms_now());done(s);}
};

class ShadowTradingSession {
public:
    ShadowTradingSession(ModelDefinition definition, RiskConfig risk, std::string registry_path = "log/models.sqlite3")
        : definition_(std::move(definition)), risk_(std::move(risk)), registry_path_(std::move(registry_path)),
          clock_(std::make_shared<SystemClock>()),
          engine_(definition_.symbol,risk_,clock_,sentum::strategy::StrategyFactory::create(definition_.strategy),shadow_db_path()),
          stream_(definition_.symbol) {}

    void start() {
        if(running_.exchange(true)) return;
        started_at_ms_=sentum::research::unix_ms_now();
        stream_.set_on_price([this](double price){if(!running_.load())return; MarketEvent e;e.type=MarketEvent::Type::Trade;e.symbol=definition_.symbol;e.price=price;e.close=price;e.timestamp=clock_->now();std::lock_guard<std::mutex> lock(mutex_);engine_.process_event(e);});
        stream_.start();
    }

    StageEvidence stop() {
        if(!running_.exchange(false)) return last_;
        stream_.stop();
        std::lock_guard<std::mutex> lock(mutex_);
        const auto metrics=MetricsCalculator::calculate(engine_.completed_trades());
        last_=evidence_from_metrics("shadow",metrics,started_at_ms_,sentum::research::unix_ms_now(),shadow_db_path());
        ModelRegistry(registry_path_).save_evidence(definition_.model_id,last_);
        write_report(last_);
        return last_;
    }
    ~ShadowTradingSession(){try{stop();}catch(...){}}
private:
    std::string shadow_db_path() const {std::filesystem::create_directories("log/shadow");return "log/shadow/"+definition_.model_id+".sqlite3";}
    void write_report(const StageEvidence&e)const{nlohmann::json j{{"model_id",definition_.model_id},{"stage",e.stage},{"started_at_ms",e.started_at_ms},{"finished_at_ms",e.finished_at_ms},{"trades",e.trades},{"net_profit",e.net_profit},{"max_drawdown",e.max_drawdown},{"profit_factor",e.profit_factor},{"win_rate",e.win_rate},{"expectancy",e.expectancy},{"sharpe",e.sharpe},{"sortino",e.sortino},{"artifact",e.artifact}};std::ofstream("log/shadow/"+definition_.model_id+"-latest.json")<<j.dump(2)<<'\n';}
    ModelDefinition definition_;RiskConfig risk_;std::string registry_path_;std::shared_ptr<SystemClock> clock_;TradeEngine engine_;BinanceWebsocketClient stream_;std::atomic<bool> running_{false};std::mutex mutex_;std::int64_t started_at_ms_=0;StageEvidence last_;
};

} // namespace sentum::promotion
