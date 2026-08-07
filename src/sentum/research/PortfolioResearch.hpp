#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <sentum/backtest/Backtest.hpp>
#include <sentum/time/Clock.hpp>
#include <sentum/trader/TradeEngine.hpp>
#include <sentum/trader/risk/PortfolioRiskManager.hpp>
#include <sentum/trader/strategy/StrategyFramework.hpp>
#include <sentum/trader/types/RiskConfig.hpp>

namespace sentum::research {

struct PortfolioDataset {
    std::string path;
    std::string symbol;
    double weight = 1.0;
};

struct PortfolioResearchConfig {
    std::vector<PortfolioDataset> datasets;
    nlohmann::json strategy = {{"type", "momentum"}, {"parameters", {{"lookback", 20}, {"entry_threshold", 0.001}}}};
    sentum::risk::PortfolioRiskConfig portfolio_risk;
    double starting_equity = 100000.0;
};

struct AssetResearchResult {
    std::string symbol;
    BacktestMetrics raw_metrics;
    std::size_t candidate_trades = 0;
    double volatility = 0.0;
};

struct PortfolioResearchSummary {
    std::vector<AssetResearchResult> assets;
    BacktestMetrics raw_combined;
    BacktestMetrics portfolio_filtered;
    std::size_t candidate_trades = 0;
    std::size_t accepted_trades = 0;
    std::size_t rejected_trades = 0;
    std::unordered_map<std::string, std::unordered_map<std::string, double>> correlations;
};

inline PortfolioResearchConfig load_portfolio_research_config(const std::string& path) {
    std::ifstream file(path);
    if (!file) throw std::runtime_error("Cannot open portfolio research config: " + path);
    nlohmann::json json; file >> json;
    PortfolioResearchConfig c;
    c.strategy = json.value("strategy", c.strategy);
    c.starting_equity = json.value("starting_equity", c.starting_equity);
    if (!(c.starting_equity > 0.0)) throw std::runtime_error("starting_equity must be positive");
    if (!json.contains("datasets") || !json.at("datasets").is_array() || json.at("datasets").empty())
        throw std::runtime_error("portfolio research requires a non-empty datasets array");
    for (const auto& item : json.at("datasets")) {
        PortfolioDataset d{item.value("path", std::string{}), item.value("symbol", std::string{}), item.value("weight", 1.0)};
        if (d.path.empty() || d.symbol.empty() || !(d.weight > 0.0)) throw std::runtime_error("invalid portfolio dataset");
        c.datasets.push_back(std::move(d));
    }
    if (json.contains("portfolio_risk")) {
        const auto& r = json.at("portfolio_risk");
        c.portfolio_risk.max_gross_exposure = r.value("max_gross_exposure", c.portfolio_risk.max_gross_exposure);
        c.portfolio_risk.max_asset_exposure = r.value("max_asset_exposure", c.portfolio_risk.max_asset_exposure);
        c.portfolio_risk.max_correlated_exposure = r.value("max_correlated_exposure", c.portfolio_risk.max_correlated_exposure);
        c.portfolio_risk.max_daily_drawdown = r.value("max_daily_drawdown", c.portfolio_risk.max_daily_drawdown);
        c.portfolio_risk.max_consecutive_losses = r.value("max_consecutive_losses", c.portfolio_risk.max_consecutive_losses);
        c.portfolio_risk.max_trades_per_hour = r.value("max_trades_per_hour", c.portfolio_risk.max_trades_per_hour);
        c.portfolio_risk.target_volatility = r.value("target_volatility", c.portfolio_risk.target_volatility);
        c.portfolio_risk.correlation_threshold = r.value("correlation_threshold", c.portfolio_risk.correlation_threshold);
    }
    return c;
}

class PortfolioResearchRunner {
public:
    explicit PortfolioResearchRunner(RiskConfig risk) : risk_(std::move(risk)) {}

    PortfolioResearchSummary run(const PortfolioResearchConfig& config) const {
        struct AssetData { PortfolioDataset cfg; std::vector<MarketEvent> events; std::vector<TradePosition> trades; double vol = 0.0; };
        std::vector<AssetData> assets;
        assets.reserve(config.datasets.size());
        std::vector<TradePosition> raw_trades;

        for (const auto& dataset : config.datasets) {
            AssetData a; a.cfg = dataset; a.events = HistoricalEventReader::read_csv(dataset.path, dataset.symbol);
            if (a.events.size() < 3) throw std::runtime_error("portfolio dataset requires at least 3 events: " + dataset.symbol);
            a.vol = realized_volatility(a.events);
            auto clock = std::make_shared<ReplayClock>();
            auto strategy = sentum::strategy::StrategyFactory::create(config.strategy);
            TradeEngine engine(dataset.symbol, risk_, clock, std::move(strategy), ":memory:");
            for (const auto& event : a.events) { clock->advance_to(event.timestamp); engine.process_event(event); }
            a.trades = engine.completed_trades();
            for (auto trade : a.trades) {
                scale_trade(trade, dataset.weight);
                raw_trades.push_back(std::move(trade));
            }
            assets.push_back(std::move(a));
        }

        PortfolioResearchSummary out;
        out.correlations = correlations(assets);
        out.raw_combined = MetricsCalculator::calculate(sorted_by_exit(raw_trades));
        out.candidate_trades = raw_trades.size();
        for (const auto& a : assets)
            out.assets.push_back({a.cfg.symbol, MetricsCalculator::calculate(a.trades), a.trades.size(), a.vol});

        std::sort(raw_trades.begin(), raw_trades.end(), [](const auto& a, const auto& b) {
            if (a.entry_time != b.entry_time) return a.entry_time < b.entry_time;
            return a.symbol < b.symbol;
        });

        sentum::risk::PortfolioRiskManager manager(config.portfolio_risk);
        double equity = config.starting_equity;
        const double day_start = equity;
        std::vector<TradePosition> open;
        std::vector<TradePosition> accepted;
        std::deque<std::chrono::system_clock::time_point> trade_times;
        std::size_t consecutive_losses = 0;

        for (auto trade : raw_trades) {
            close_until(trade.entry_time, open, accepted, equity, consecutive_losses);
            sentum::risk::PortfolioRiskSnapshot snapshot;
            snapshot.equity = equity;
            snapshot.day_start_equity = day_start;
            snapshot.correlations = out.correlations;
            snapshot.consecutive_losses = consecutive_losses;
            snapshot.trade_times = trade_times;
            for (const auto& a : assets) snapshot.annualized_volatility[a.cfg.symbol] = a.vol;
            for (const auto& p : open) snapshot.positions.push_back({p.symbol, p.entry_price * p.quantity});
            const double proposed = trade.entry_price * trade.quantity;
            const auto decision = manager.approve(trade.symbol, proposed, snapshot, trade.entry_time);
            if (!decision.approved) { ++out.rejected_trades; continue; }
            scale_trade(trade, decision.size_multiplier);
            open.push_back(std::move(trade));
            trade_times.push_back(open.back().entry_time);
            while (!trade_times.empty() && trade_times.front() < open.back().entry_time - std::chrono::hours(1)) trade_times.pop_front();
            ++out.accepted_trades;
        }
        close_until(std::chrono::system_clock::time_point::max(), open, accepted, equity, consecutive_losses);
        out.portfolio_filtered = MetricsCalculator::calculate(sorted_by_exit(accepted));
        return out;
    }

    static nlohmann::json to_json(const PortfolioResearchSummary& s) {
        auto metrics = [](const BacktestMetrics& m) { return nlohmann::json{{"trades",m.trades},{"net_profit",m.net_profit},{"max_drawdown",m.max_drawdown},{"profit_factor",std::isfinite(m.profit_factor)?m.profit_factor:0.0},{"win_rate",m.win_rate},{"expectancy",m.expectancy},{"sharpe",m.sharpe},{"sortino",m.sortino},{"fee_share",m.fee_share}}; };
        nlohmann::json j{{"candidate_trades",s.candidate_trades},{"accepted_trades",s.accepted_trades},{"rejected_trades",s.rejected_trades},{"raw_combined",metrics(s.raw_combined)},{"portfolio_filtered",metrics(s.portfolio_filtered)},{"assets",nlohmann::json::array()},{"correlations",s.correlations}};
        for (const auto& a : s.assets) j["assets"].push_back({{"symbol",a.symbol},{"candidate_trades",a.candidate_trades},{"volatility",a.volatility},{"metrics",metrics(a.raw_metrics)}});
        return j;
    }

    static void write_artifact(const PortfolioResearchSummary& summary, const std::string& path = "log/portfolio_research_latest.json") {
        const auto parent = std::filesystem::path(path).parent_path(); if (!parent.empty()) std::filesystem::create_directories(parent);
        const std::string tmp = path + ".tmp"; { std::ofstream f(tmp, std::ios::trunc); if (!f) throw std::runtime_error("cannot write portfolio research artifact"); f << to_json(summary).dump(2) << '\n'; }
        std::error_code ec; std::filesystem::remove(path, ec); ec.clear(); std::filesystem::rename(tmp, path, ec); if (ec) throw std::runtime_error("cannot publish portfolio research artifact: " + ec.message());
    }

private:
    template <typename AssetVector>
    static std::unordered_map<std::string, std::unordered_map<std::string, double>> correlations(const AssetVector& assets) {
        std::unordered_map<std::string, std::unordered_map<std::string, double>> out;
        for (const auto& a : assets) for (const auto& b : assets) out[a.cfg.symbol][b.cfg.symbol] = correlation(a.events, b.events);
        return out;
    }

    static double correlation(const std::vector<MarketEvent>& a, const std::vector<MarketEvent>& b) {
        const std::size_t n = std::min(a.size(), b.size()); if (n < 3) return 0.0;
        std::vector<double> x, y; x.reserve(n-1); y.reserve(n-1);
        for (std::size_t i=1;i<n;++i) if (a[i-1].price>0&&a[i].price>0&&b[i-1].price>0&&b[i].price>0) { x.push_back(std::log(a[i].price/a[i-1].price)); y.push_back(std::log(b[i].price/b[i-1].price)); }
        if (x.size()<2) return 0.0; const double mx=std::accumulate(x.begin(),x.end(),0.0)/x.size(), my=std::accumulate(y.begin(),y.end(),0.0)/y.size(); double cov=0,vx=0,vy=0; for(std::size_t i=0;i<x.size();++i){const double dx=x[i]-mx,dy=y[i]-my;cov+=dx*dy;vx+=dx*dx;vy+=dy*dy;} return vx>0&&vy>0?cov/std::sqrt(vx*vy):0.0;
    }

    static double realized_volatility(const std::vector<MarketEvent>& events) {
        if (events.size()<3) return 0.0; std::vector<double> r; r.reserve(events.size()-1); for(std::size_t i=1;i<events.size();++i) if(events[i-1].price>0&&events[i].price>0) r.push_back(std::log(events[i].price/events[i-1].price)); if(r.size()<2)return 0.0; const double mean=std::accumulate(r.begin(),r.end(),0.0)/r.size();double var=0;for(double x:r)var+=(x-mean)*(x-mean);return std::sqrt(var/(r.size()-1))*std::sqrt(365.0*24.0*60.0*60.0);
    }

    static void scale_trade(TradePosition& t, double multiplier) {
        t.quantity *= multiplier; t.gross_profit *= multiplier; t.net_profit *= multiplier; t.fee_entry *= multiplier; t.fee_exit *= multiplier; t.capital_at_risk *= multiplier;
    }

    static std::vector<TradePosition> sorted_by_exit(std::vector<TradePosition> v) {
        std::sort(v.begin(),v.end(),[](const auto&a,const auto&b){return a.exit_time<b.exit_time;});return v;
    }

    static void close_until(std::chrono::system_clock::time_point at, std::vector<TradePosition>& open,
                            std::vector<TradePosition>& accepted, double& equity, std::size_t& losses) {
        auto it=open.begin();while(it!=open.end()){if(it->exit_time<=at){equity+=it->net_profit;losses=it->net_profit<0?losses+1:0;accepted.push_back(*it);it=open.erase(it);}else ++it;}
    }

    RiskConfig risk_;
};

} // namespace sentum::research
