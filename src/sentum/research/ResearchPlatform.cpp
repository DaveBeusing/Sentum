#include <sentum/research/ResearchPlatform.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <numeric>
#include <random>
#include <stdexcept>
#include <thread>
#include <utility>

#include <sentum/time/Clock.hpp>
#include <sentum/trader/TradeEngine.hpp>
#include <sentum/trader/strategy/MomentumStrategy.hpp>

namespace sentum::research {
namespace {

struct SliceResult {
    BacktestMetrics metrics;
    std::vector<TradePosition> trades;
};

template <typename T>
std::vector<T> value_or(const nlohmann::json& object, const char* key, std::vector<T> fallback) {
    if (!object.contains(key)) return fallback;
    auto values = object.at(key).get<std::vector<T>>();
    if (values.empty()) throw std::runtime_error(std::string("Research grid '") + key + "' cannot be empty");
    return values;
}

void validate_positive(const std::vector<double>& values, const char* key, bool allow_zero = false) {
    for (double value : values) {
        if (!std::isfinite(value) || value < 0.0 || (!allow_zero && value == 0.0))
            throw std::runtime_error(std::string("Invalid research parameter in '") + key + "'");
    }
}

void validate_lookbacks(const std::vector<std::size_t>& values) {
    for (auto value : values)
        if (value < 2) throw std::runtime_error("Research lookback must be >= 2");
}

std::size_t checked_trial_count(const ResearchConfig& config) {
    if (config.max_trials == 0) throw std::runtime_error("max_trials must be >= 1");
    const std::size_t dimensions[] = {config.lookbacks.size(), config.entry_thresholds.size(), config.stop_losses.size(), config.take_profits.size(), config.slippages.size()};
    std::size_t count = 1;
    for (auto dimension : dimensions) {
        if (dimension == 0) throw std::runtime_error("Research parameter grid cannot be empty");
        if (count > config.max_trials / dimension) throw std::runtime_error("Research grid exceeds max_trials");
        count *= dimension;
    }
    return count;
}

void apply_trial_risk(RiskConfig& risk, const ParameterSet& p) {
    risk.stop_loss_percent = p.stop_loss_percent;
    risk.take_profit_percent = p.take_profit_percent;
    risk.slippage_percent = p.slippage_percent;
}

SliceResult run_slice(const std::vector<MarketEvent>& events, std::size_t begin, std::size_t end,
                      const ParameterSet& p, RiskConfig risk, const std::string& symbol, bool warmup) {
    if (begin >= end || end > events.size()) return {};
    apply_trial_risk(risk, p);
    auto strategy = std::make_unique<MomentumStrategy>(p.lookback, p.entry_threshold);
    if (warmup && begin > 0) {
        const std::size_t n = std::min<std::size_t>(begin, p.lookback + 1);
        for (std::size_t i = begin - n; i < begin; ++i) strategy->on_price(events[i].price, events[i].timestamp);
    }
    auto clock = std::make_shared<ReplayClock>();
    TradeEngine engine(symbol, risk, clock, std::move(strategy), ":memory:");
    for (std::size_t i = begin; i < end; ++i) { clock->advance_to(events[i].timestamp); engine.process_event(events[i]); }
    SliceResult r; r.trades = engine.completed_trades(); r.metrics = MetricsCalculator::calculate(r.trades); return r;
}

double finite_or_zero(double v) { return std::isfinite(v) ? v : 0.0; }

double quantile(std::vector<double> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const double pos = std::clamp(p, 0.0, 1.0) * static_cast<double>(v.size() - 1);
    const auto lo = static_cast<std::size_t>(std::floor(pos)); const auto hi = static_cast<std::size_t>(std::ceil(pos));
    if (lo == hi) return v[lo]; const double w = pos - static_cast<double>(lo); return v[lo] * (1.0 - w) + v[hi] * w;
}

ConfidenceInterval make_interval(std::vector<double> values, double confidence) {
    const double a = (1.0 - confidence) * 0.5;
    return {quantile(values, a), quantile(values, 0.5), quantile(values, 1.0 - a)};
}

MonteCarloSummary monte_carlo(const std::vector<TradePosition>& trades, std::size_t samples, double confidence, std::uint64_t seed) {
    MonteCarloSummary out; out.samples = samples;
    if (trades.empty() || samples == 0) return out;
    std::vector<double> pnl; for (const auto& t : trades) pnl.push_back(t.net_profit);
    std::mt19937_64 rng(seed); std::uniform_int_distribution<std::size_t> pick(0, pnl.size() - 1);
    std::vector<double> profits, drawdowns; profits.reserve(samples); drawdowns.reserve(samples); std::size_t losses = 0;
    for (std::size_t s = 0; s < samples; ++s) {
        double equity = 0.0, peak = 0.0, max_dd = 0.0;
        for (std::size_t i = 0; i < pnl.size(); ++i) { equity += pnl[pick(rng)]; peak = std::max(peak, equity); max_dd = std::max(max_dd, peak - equity); }
        if (equity < 0.0) ++losses; profits.push_back(equity); drawdowns.push_back(max_dd);
    }
    out.net_profit = make_interval(std::move(profits), confidence); out.max_drawdown = make_interval(std::move(drawdowns), confidence);
    out.probability_of_loss = static_cast<double>(losses) / static_cast<double>(samples); return out;
}

ConfidenceInterval bootstrap_profit(const std::vector<TradePosition>& trades, std::size_t samples, double confidence, std::uint64_t seed) {
    if (trades.empty() || samples == 0) return {};
    std::mt19937_64 rng(seed ^ 0xB00757A9ULL); std::uniform_int_distribution<std::size_t> pick(0, trades.size() - 1);
    std::vector<double> values; values.reserve(samples);
    for (std::size_t s = 0; s < samples; ++s) { double sum = 0.0; for (std::size_t i = 0; i < trades.size(); ++i) sum += trades[pick(rng)].net_profit; values.push_back(sum); }
    return make_interval(std::move(values), confidence);
}

double deflated_sharpe(double sharpe, std::size_t trades, std::size_t trials) {
    if (trades < 2 || trials < 2) return sharpe;
    return sharpe - std::sqrt(2.0 * std::log(static_cast<double>(trials))) / std::sqrt(static_cast<double>(trades));
}

double distance(const ParameterSet& a, const ParameterSet& b, const ParameterSet& lo, const ParameterSet& hi) {
    const auto n = [](double x, double y, double l, double h) { return h > l ? std::abs(x - y) / (h - l) : 0.0; };
    return n(a.lookback, b.lookback, lo.lookback, hi.lookback) + n(a.entry_threshold,b.entry_threshold,lo.entry_threshold,hi.entry_threshold) +
           n(a.stop_loss_percent,b.stop_loss_percent,lo.stop_loss_percent,hi.stop_loss_percent) + n(a.take_profit_percent,b.take_profit_percent,lo.take_profit_percent,hi.take_profit_percent) +
           n(a.slippage_percent,b.slippage_percent,lo.slippage_percent,hi.slippage_percent);
}

void calculate_stability(std::vector<TrialResult>& results) {
    if (results.empty()) return;
    ParameterSet lo = results.front().parameters, hi = lo;
    for (const auto& r : results) {
        lo.lookback = std::min(lo.lookback,r.parameters.lookback); hi.lookback = std::max(hi.lookback,r.parameters.lookback);
        lo.entry_threshold=std::min(lo.entry_threshold,r.parameters.entry_threshold); hi.entry_threshold=std::max(hi.entry_threshold,r.parameters.entry_threshold);
        lo.stop_loss_percent=std::min(lo.stop_loss_percent,r.parameters.stop_loss_percent); hi.stop_loss_percent=std::max(hi.stop_loss_percent,r.parameters.stop_loss_percent);
        lo.take_profit_percent=std::min(lo.take_profit_percent,r.parameters.take_profit_percent); hi.take_profit_percent=std::max(hi.take_profit_percent,r.parameters.take_profit_percent);
        lo.slippage_percent=std::min(lo.slippage_percent,r.parameters.slippage_percent); hi.slippage_percent=std::max(hi.slippage_percent,r.parameters.slippage_percent);
    }
    for (auto& r : results) {
        std::vector<std::pair<double,double>> neighbors;
        for (const auto& o : results) if (o.trial_id != r.trial_id) neighbors.emplace_back(distance(r.parameters,o.parameters,lo,hi),std::abs(r.validation_score-o.validation_score));
        std::sort(neighbors.begin(),neighbors.end()); const std::size_t k=std::min<std::size_t>(5,neighbors.size());
        double wd=0.0,w=0.0; for(std::size_t i=0;i<k;++i){const double wi=1.0/(0.05+neighbors[i].first);wd+=wi*neighbors[i].second;w+=wi;}
        r.parameter_stability_score = k ? 1.0/(1.0+wd/std::max(1e-12,w)) : 1.0;
    }
}

std::string regime(const std::vector<MarketEvent>& events, std::chrono::system_clock::time_point entry) {
    auto it=std::lower_bound(events.begin(),events.end(),entry,[](const MarketEvent&e,const auto&t){return e.timestamp<t;});
    if(it==events.begin()) return "unknown"; const std::size_t idx=std::distance(events.begin(),it), begin=idx>20?idx-20:0; if(idx-begin<3)return "unknown";
    const double first=events[begin].price,last=events[idx-1].price; if(first<=0||last<=0)return "unknown"; const double trend=last/first-1.0;
    std::vector<double> rs; for(std::size_t i=begin+1;i<idx;++i)if(events[i-1].price>0&&events[i].price>0)rs.push_back(std::log(events[i].price/events[i-1].price));
    const double mean=rs.empty()?0.0:std::accumulate(rs.begin(),rs.end(),0.0)/rs.size(); double var=0;for(double x:rs)var+=(x-mean)*(x-mean); const double vol=rs.size()>1?std::sqrt(var/(rs.size()-1)):0;
    if(vol>0.003)return "high_volatility"; if(trend>0.005)return "trending_up"; if(trend<-0.005)return "trending_down"; return "ranging";
}

std::vector<RegimeMetrics> regime_metrics(const std::vector<MarketEvent>& events,const std::vector<TradePosition>& trades){
    std::vector<RegimeMetrics> out; for(const std::string name:{"trending_up","trending_down","ranging","high_volatility","unknown"}){std::vector<TradePosition>b;for(const auto&t:trades)if(regime(events,t.entry_time)==name)b.push_back(t);if(!b.empty())out.push_back({name,MetricsCalculator::calculate(b)});}return out;
}

nlohmann::json metrics_json(const BacktestMetrics&m){return{{"net_profit",finite_or_zero(m.net_profit)},{"max_drawdown",finite_or_zero(m.max_drawdown)},{"profit_factor",finite_or_zero(m.profit_factor)},{"win_rate",finite_or_zero(m.win_rate)},{"expectancy",finite_or_zero(m.expectancy)},{"sharpe",finite_or_zero(m.sharpe)},{"sortino",finite_or_zero(m.sortino)},{"fee_share",finite_or_zero(m.fee_share)},{"slippage_sensitivity",finite_or_zero(m.slippage_sensitivity)},{"trades",m.trades}};}
nlohmann::json interval_json(const ConfidenceInterval&v){return{{"lower",finite_or_zero(v.lower)},{"median",finite_or_zero(v.median)},{"upper",finite_or_zero(v.upper)}};}
nlohmann::json parameter_json(const ParameterSet&p){return{{"lookback",p.lookback},{"entry_threshold",p.entry_threshold},{"stop_loss_percent",p.stop_loss_percent},{"take_profit_percent",p.take_profit_percent},{"slippage_percent",p.slippage_percent}};}
nlohmann::json trial_json(const TrialResult&t){return{{"trial_id",t.trial_id},{"parameters",parameter_json(t.parameters)},{"train",metrics_json(t.train)},{"validation",metrics_json(t.validation)},{"train_score",finite_or_zero(t.train_score)},{"validation_score",finite_or_zero(t.validation_score)},{"overfit_gap",finite_or_zero(t.overfit_gap)},{"parameter_stability_score",finite_or_zero(t.parameter_stability_score)},{"deflated_sharpe",finite_or_zero(t.deflated_sharpe)},{"eligible",t.eligible}};}

} // namespace

ResearchConfig load_research_config(const std::string& path) {
    std::ifstream file(path); if(!file)throw std::runtime_error("Cannot open research config: "+path); nlohmann::json json;file>>json; ResearchConfig c;
    c.dataset=json.value("dataset",std::string{});c.symbol=json.value("symbol",std::string{});c.objective=json.value("objective",std::string("sharpe"));c.train_fraction=json.value("train_fraction",0.60);c.holdout_fraction=json.value("holdout_fraction",0.15);c.walk_forward_folds=json.value("walk_forward_folds",std::size_t{3});c.purge_events=json.value("purge_events",std::size_t{0});c.embargo_events=json.value("embargo_events",std::size_t{0});c.min_validation_trades=json.value("min_validation_trades",std::size_t{10});c.max_trials=json.value("max_trials",std::size_t{5000});c.leaderboard_size=json.value("leaderboard_size",std::size_t{25});c.monte_carlo_samples=json.value("monte_carlo_samples",std::size_t{2000});c.bootstrap_samples=json.value("bootstrap_samples",std::size_t{2000});c.confidence_level=json.value("confidence_level",0.95);c.random_seed=json.value("random_seed",static_cast<std::uint64_t>(0x53454e54554dULL));c.parallelism=json.value("parallelism",std::size_t{0});
    const auto grid=json.contains("grid")?json.at("grid"):nlohmann::json::object();if(!grid.is_object())throw std::runtime_error("Research grid must be a JSON object");c.lookbacks=value_or<std::size_t>(grid,"lookback",{10,20,40});c.entry_thresholds=value_or<double>(grid,"entry_threshold",{0.0005,0.001,0.002});c.stop_losses=value_or<double>(grid,"stop_loss_percent",{});c.take_profits=value_or<double>(grid,"take_profit_percent",{});c.slippages=value_or<double>(grid,"slippage_percent",{});
    if(c.dataset.empty()||c.symbol.empty())throw std::runtime_error("Research config requires dataset and symbol");if(!(c.train_fraction>0.10&&c.train_fraction<0.90))throw std::runtime_error("train_fraction must be between 0.10 and 0.90");if(!(c.holdout_fraction>0.0&&c.holdout_fraction<0.40))throw std::runtime_error("holdout_fraction must be between 0 and 0.40");if(c.train_fraction+c.holdout_fraction>=0.95)throw std::runtime_error("train_fraction + holdout_fraction leaves insufficient validation data");if(c.walk_forward_folds==0||c.leaderboard_size==0)throw std::runtime_error("folds and leaderboard_size must be >= 1");if(!(c.confidence_level>0.50&&c.confidence_level<1.0))throw std::runtime_error("confidence_level must be between 0.50 and 1.0");validate_lookbacks(c.lookbacks);validate_positive(c.entry_thresholds,"entry_threshold",true);if(!c.stop_losses.empty())validate_positive(c.stop_losses,"stop_loss_percent");if(!c.take_profits.empty())validate_positive(c.take_profits,"take_profit_percent");if(!c.slippages.empty())validate_positive(c.slippages,"slippage_percent",true);return c;
}

ResearchRunner::ResearchRunner(RiskConfig base_risk):base_risk_(base_risk){}
double ResearchRunner::score(const BacktestMetrics&m,const std::string&o){if(o=="sharpe")return m.sharpe;if(o=="sortino")return m.sortino;if(o=="net_profit")return m.net_profit;if(o=="profit_factor")return std::isfinite(m.profit_factor)?m.profit_factor:1000000.0;if(o=="expectancy")return m.expectancy;if(o=="risk_adjusted_profit")return m.net_profit/(1.0+std::max(0.0,m.max_drawdown));throw std::runtime_error("Unsupported research objective: "+o);}

ResearchSummary ResearchRunner::run(const ResearchConfig& input){return ResearchSummary{};}

ResearchSummary ResearchRunner::run(const ResearchConfig& input) const {
    ResearchConfig c=input;if(c.stop_losses.empty())c.stop_losses={base_risk_.stop_loss_percent};if(c.take_profits.empty())c.take_profits={base_risk_.take_profit_percent};if(c.slippages.empty())c.slippages={base_risk_.slippage_percent};const auto trial_count=checked_trial_count(c);
    const auto events=HistoricalEventReader::read_csv(c.dataset,c.symbol);if(events.size()<50)throw std::runtime_error("Research robustness requires at least 50 events");const std::size_t holdout_n=std::max<std::size_t>(1,events.size()*c.holdout_fraction),research_end=events.size()-holdout_n;std::size_t initial_train=std::clamp<std::size_t>(events.size()*c.train_fraction,2,research_end-1);const std::size_t remaining=research_end-initial_train,folds=std::min(c.walk_forward_folds,remaining);if(!folds)throw std::runtime_error("Research dataset leaves no validation events");const std::size_t fold_width=std::max<std::size_t>(1,remaining/folds);
    std::vector<ParameterSet> ps;ps.reserve(trial_count);for(auto l:c.lookbacks)for(double e:c.entry_thresholds)for(double s:c.stop_losses)for(double t:c.take_profits)for(double sl:c.slippages)ps.push_back({l,e,s,t,sl});
    ResearchSummary out;out.dataset=c.dataset;out.symbol=c.symbol;out.objective=c.objective;out.generated_at_ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();out.events=events.size();out.research_events=research_end;out.holdout_events=holdout_n;out.folds=folds;out.trials=trial_count;out.results.resize(trial_count);
    std::atomic<std::size_t> next{0};const std::size_t requested=c.parallelism?c.parallelism:std::max(1u,std::thread::hardware_concurrency()),workers=std::max<std::size_t>(1,std::min<std::size_t>(requested,trial_count));std::vector<std::thread> pool;
    for(std::size_t w=0;w<workers;++w)pool.emplace_back([&]{while(true){const std::size_t idx=next.fetch_add(1);if(idx>=ps.size())break;const auto p=ps[idx];std::vector<TradePosition> train,oos;for(std::size_t f=0;f<folds;++f){const std::size_t boundary=initial_train+f*fold_width,ve=f+1==folds?research_end:std::min(research_end,initial_train+(f+1)*fold_width),te=boundary>c.purge_events?boundary-c.purge_events:0,vb=std::min(ve,boundary+c.embargo_events);if(te){auto r=run_slice(events,0,te,p,base_risk_,c.symbol,false);train.insert(train.end(),r.trades.begin(),r.trades.end());}if(vb<ve){auto r=run_slice(events,vb,ve,p,base_risk_,c.symbol,true);oos.insert(oos.end(),r.trades.begin(),r.trades.end());}}TrialResult r;r.trial_id=idx+1;r.parameters=p;r.train=MetricsCalculator::calculate(train);r.validation=MetricsCalculator::calculate(oos);r.train_score=score(r.train,c.objective);r.validation_score=score(r.validation,c.objective);r.overfit_gap=r.train_score-r.validation_score;r.deflated_sharpe=deflated_sharpe(r.validation.sharpe,r.validation.trades,trial_count);r.eligible=r.validation.trades>=c.min_validation_trades;out.results[idx]=std::move(r);}});for(auto&t:pool)t.join();
    calculate_stability(out.results);out.leaderboard=out.results;std::stable_sort(out.leaderboard.begin(),out.leaderboard.end(),[](const auto&a,const auto&b){if(a.eligible!=b.eligible)return a.eligible>b.eligible;if(a.validation_score!=b.validation_score)return a.validation_score>b.validation_score;if(a.deflated_sharpe!=b.deflated_sharpe)return a.deflated_sharpe>b.deflated_sharpe;if(a.parameter_stability_score!=b.parameter_stability_score)return a.parameter_stability_score>b.parameter_stability_score;return std::abs(a.overfit_gap)<std::abs(b.overfit_gap);});if(out.leaderboard.size()>c.leaderboard_size)out.leaderboard.resize(c.leaderboard_size);
    auto selected=std::find_if(out.leaderboard.begin(),out.leaderboard.end(),[](const auto&t){return t.eligible;});if(selected!=out.leaderboard.end()){out.selected_parameters=selected->parameters;const auto h=run_slice(events,research_end,events.size(),selected->parameters,base_risk_,c.symbol,true);out.final_holdout=h.metrics;out.final_holdout_score=score(h.metrics,c.objective);out.holdout_evaluated=true;out.bootstrap_net_profit=bootstrap_profit(h.trades,c.bootstrap_samples,c.confidence_level,c.random_seed);out.monte_carlo=monte_carlo(h.trades,c.monte_carlo_samples,c.confidence_level,c.random_seed);out.holdout_regimes=regime_metrics(events,h.trades);}return out;
}

nlohmann::json ResearchRunner::to_json(const ResearchSummary&s){nlohmann::json j{{"dataset",s.dataset},{"symbol",s.symbol},{"objective",s.objective},{"generated_at_ms",s.generated_at_ms},{"events",s.events},{"research_events",s.research_events},{"holdout_events",s.holdout_events},{"folds",s.folds},{"trials",s.trials},{"holdout_evaluated",s.holdout_evaluated},{"leaderboard",nlohmann::json::array()}};for(const auto&t:s.leaderboard)j["leaderboard"].push_back(trial_json(t));if(s.holdout_evaluated){j["selected_parameters"]=parameter_json(s.selected_parameters);j["final_holdout"]=metrics_json(s.final_holdout);j["final_holdout_score"]=finite_or_zero(s.final_holdout_score);j["bootstrap_net_profit"]=interval_json(s.bootstrap_net_profit);j["monte_carlo"]={{"samples",s.monte_carlo.samples},{"net_profit",interval_json(s.monte_carlo.net_profit)},{"max_drawdown",interval_json(s.monte_carlo.max_drawdown)},{"probability_of_loss",finite_or_zero(s.monte_carlo.probability_of_loss)}};j["holdout_regimes"]=nlohmann::json::array();for(const auto&r:s.holdout_regimes)j["holdout_regimes"].push_back({{"regime",r.regime},{"metrics",metrics_json(r.metrics)}});}return j;}

void ResearchRunner::write_artifacts(const ResearchSummary&s,const std::string&json_path,const std::string&csv_path){const auto jp=std::filesystem::path(json_path).parent_path(),cp=std::filesystem::path(csv_path).parent_path();if(!jp.empty())std::filesystem::create_directories(jp);if(!cp.empty())std::filesystem::create_directories(cp);const std::string tmp=json_path+".tmp";{std::ofstream f(tmp,std::ios::trunc);if(!f)throw std::runtime_error("Cannot write research JSON artifact");f<<to_json(s).dump(2)<<'\n';}std::error_code ec;std::filesystem::remove(json_path,ec);ec.clear();std::filesystem::rename(tmp,json_path,ec);if(ec)throw std::runtime_error("Cannot publish research JSON artifact: "+ec.message());std::ofstream csv(csv_path,std::ios::trunc);if(!csv)throw std::runtime_error("Cannot write research CSV artifact");csv<<"trial_id,lookback,entry_threshold,stop_loss_percent,take_profit_percent,slippage_percent,eligible,train_score,validation_score,overfit_gap,parameter_stability_score,deflated_sharpe,train_trades,validation_trades,train_net_profit,validation_net_profit,validation_max_drawdown,validation_sharpe,validation_sortino\n";csv<<std::setprecision(17);for(const auto&t:s.results)csv<<t.trial_id<<','<<t.parameters.lookback<<','<<t.parameters.entry_threshold<<','<<t.parameters.stop_loss_percent<<','<<t.parameters.take_profit_percent<<','<<t.parameters.slippage_percent<<','<<(t.eligible?1:0)<<','<<t.train_score<<','<<t.validation_score<<','<<t.overfit_gap<<','<<t.parameter_stability_score<<','<<t.deflated_sharpe<<','<<t.train.trades<<','<<t.validation.trades<<','<<t.train.net_profit<<','<<t.validation.net_profit<<','<<t.validation.max_drawdown<<','<<t.validation.sharpe<<','<<t.validation.sortino<<'\n';}

} // namespace sentum::research
