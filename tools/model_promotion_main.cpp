#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>

#include <sentum/research/ModelPromotion.hpp>
#include <sentum/research/ModelSourceValidation.hpp>
#include <sentum/trader/utils/RiskConfigLoader.hpp>

namespace {
std::atomic<bool> stop_requested{false};
void signal_handler(int) noexcept { stop_requested.store(true, std::memory_order_relaxed); }
std::string read_all(const std::string& path){std::ifstream f(path);if(!f)throw std::runtime_error("Cannot open file: "+path);return {std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>()};}
void usage(){std::cerr<<"Usage:\n  sentum_model register <model.json>\n  sentum_model status <model.json>\n  sentum_model shadow <model.json>\n  sentum_model record-evidence <model.json> <paper|testnet> <metrics.json>\n  sentum_model promote <model.json> <shadow|paper|testnet> I_APPROVE_MODEL_PROMOTION\n";}
}

int main(int argc,char**argv){std::signal(SIGINT,signal_handler);std::signal(SIGTERM,signal_handler);try{
    if(argc<3){usage();return EXIT_FAILURE;}const std::string cmd=argv[1],path=argv[2];const auto model=sentum::promotion::load_model_definition(path);sentum::promotion::ModelRegistry registry;
    if(cmd=="register"){const auto research=sentum::promotion::validate_source_experiment(model);registry.register_model(model,read_all(path));registry.save_evidence(model.model_id,research);std::cout<<"Registered model "<<model.model_id<<" at research stage with validated source experiment "<<model.source_experiment_run<<"\n";return EXIT_SUCCESS;}
    if(cmd=="status"){std::cout<<registry.snapshot(model.model_id).dump(2)<<'\n';return EXIT_SUCCESS;}
    if(cmd=="shadow"){
        const auto stage=registry.stage(model.model_id);if(stage!="shadow")throw std::runtime_error("Model must be promoted to shadow before shadow runtime starts; current stage="+stage);
        auto risk=load_risk_config(model.risk_config);sentum::promotion::ShadowTradingSession session(model,risk);session.start();std::cout<<"Shadow trading "<<model.model_id<<" on live "<<model.symbol<<" market data. No exchange orders are submitted. Ctrl+C to stop.\n";
        while(!stop_requested.load(std::memory_order_relaxed))std::this_thread::sleep_for(std::chrono::milliseconds(200));const auto e=session.stop();std::cout<<"Shadow evidence: trades="<<e.trades<<" net="<<e.net_profit<<" sharpe="<<e.sharpe<<" maxDD="<<e.max_drawdown<<'\n';return EXIT_SUCCESS;
    }
    if(cmd=="record-evidence"&&argc==5){const std::string stage=argv[3];if(stage!="paper"&&stage!="testnet")throw std::runtime_error("Evidence stage must be paper or testnet");if(registry.stage(model.model_id)!=stage)throw std::runtime_error("Evidence may only be recorded for the model's current stage");nlohmann::json j;std::ifstream(argv[4])>>j;sentum::promotion::StageEvidence e;e.stage=stage;e.started_at_ms=j.value("started_at_ms",std::int64_t{0});e.finished_at_ms=j.value("finished_at_ms",sentum::research::unix_ms_now());e.trades=j.value("trades",std::size_t{0});e.net_profit=j.value("net_profit",0.0);e.max_drawdown=j.value("max_drawdown",0.0);e.profit_factor=j.value("profit_factor",0.0);e.win_rate=j.value("win_rate",0.0);e.expectancy=j.value("expectancy",0.0);e.sharpe=j.value("sharpe",0.0);e.sortino=j.value("sortino",0.0);e.artifact=argv[4];registry.save_evidence(model.model_id,e);std::cout<<"Recorded "<<stage<<" evidence for "<<model.model_id<<'\n';return EXIT_SUCCESS;}
    if(cmd=="promote"&&argc==5){const auto target=sentum::promotion::parse_stage(argv[3]);registry.promote(model,target,argv[4]);std::cout<<"Promoted "<<model.model_id<<" to "<<argv[3]<<'\n';return EXIT_SUCCESS;}
    usage();return EXIT_FAILURE;
}catch(const std::exception&e){std::cerr<<"[FATAL] "<<e.what()<<'\n';return EXIT_FAILURE;}}
