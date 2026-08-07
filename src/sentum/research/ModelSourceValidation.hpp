#pragma once

#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <sentum/research/ModelPromotion.hpp>
#include <sentum/trader/utils/RiskConfigLoader.hpp>

namespace sentum::promotion {

inline bool nearly_equal(double a,double b){return std::abs(a-b)<=1e-12*std::max({1.0,std::abs(a),std::abs(b)});}

inline StageEvidence validate_source_experiment(const ModelDefinition& model,
                                                const std::string& registry_path = "log/experiments.sqlite3") {
    sqlite3* db=nullptr;if(sqlite3_open_v2(registry_path.c_str(),&db,SQLITE_OPEN_READONLY|SQLITE_OPEN_FULLMUTEX,nullptr)!=SQLITE_OK){if(db)sqlite3_close(db);throw std::runtime_error("Cannot open experiment registry: "+registry_path);}sqlite3_stmt* stmt=nullptr;
    const char* sql="SELECT status,git_commit,config_sha256,output_directory FROM research_runs WHERE run_id=?;";if(sqlite3_prepare_v2(db,sql,-1,&stmt,nullptr)!=SQLITE_OK){sqlite3_close(db);throw std::runtime_error("Cannot query source experiment");}sqlite3_bind_text(stmt,1,model.source_experiment_run.c_str(),-1,SQLITE_TRANSIENT);if(sqlite3_step(stmt)!=SQLITE_ROW){sqlite3_finalize(stmt);sqlite3_close(db);throw std::runtime_error("Source experiment run not found");}
    auto text=[&](int i){const auto*p=sqlite3_column_text(stmt,i);return p?std::string(reinterpret_cast<const char*>(p)):std::string{};};const std::string status=text(0),git=text(1),config_hash=text(2),output=text(3);sqlite3_finalize(stmt);sqlite3_close(db);
    if(status!="completed")throw std::runtime_error("Source experiment is not completed");if(!model.source_git_commit.empty()&&git!=model.source_git_commit)throw std::runtime_error("Source experiment Git commit mismatch");if(!model.source_config_sha256.empty()&&config_hash!=model.source_config_sha256)throw std::runtime_error("Source experiment config hash mismatch");

    const auto research_path=std::filesystem::path(output)/"research.json";std::ifstream file(research_path);if(!file)throw std::runtime_error("Source experiment does not contain research.json; promotion currently requires a single-asset research run");nlohmann::json r;file>>r;
    if(!r.value("holdout_evaluated",false)||!r.contains("final_holdout")||!r.contains("selected_parameters"))throw std::runtime_error("Source experiment has no selected final Holdout model");

    // Phase-10/11 single-asset research optimizes MomentumStrategy. A promoted model must be exactly that researched candidate.
    if(model.strategy.value("type",std::string{})!="momentum")throw std::runtime_error("Source experiment is momentum research; promoted strategy type must be momentum");
    const auto params=model.strategy.value("parameters",nlohmann::json::object());const auto& selected=r.at("selected_parameters");
    if(params.value("lookback",std::size_t{0})!=selected.value("lookback",std::size_t{0})||
       !nearly_equal(params.value("entry_threshold",-1.0),selected.value("entry_threshold",-2.0)))
        throw std::runtime_error("Model strategy parameters do not match source experiment selected_parameters");

    const auto risk=load_risk_config(model.risk_config);
    if(!nearly_equal(risk.stop_loss_percent,selected.value("stop_loss_percent",-1.0))||
       !nearly_equal(risk.take_profit_percent,selected.value("take_profit_percent",-1.0))||
       !nearly_equal(risk.slippage_percent,selected.value("slippage_percent",-1.0)))
        throw std::runtime_error("Current risk configuration does not match researched stop/take-profit/slippage parameters");

    const auto&m=r.at("final_holdout");StageEvidence e;e.stage="research";e.started_at_ms=0;e.finished_at_ms=r.value("generated_at_ms",std::int64_t{0});e.trades=m.value("trades",std::size_t{0});e.net_profit=m.value("net_profit",0.0);e.max_drawdown=m.value("max_drawdown",0.0);e.profit_factor=m.value("profit_factor",0.0);e.win_rate=m.value("win_rate",0.0);e.expectancy=m.value("expectancy",0.0);e.sharpe=m.value("sharpe",0.0);e.sortino=m.value("sortino",0.0);e.artifact=research_path.string();return e;
}

} // namespace sentum::promotion
