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
    bool volatility_available = false;
};

struct PortfolioResearchSummary {
    std::vector<AssetResearchResult> assets;
    BacktestMetrics raw_combined;
    BacktestMetrics portfolio_filtered;
    std::size_t candidate_trades = 0;
    std::size_t accepted_trades = 0;
    std::size_t rejected_trades = 0;
    std::unordered_map<std::string, std::unordered_map<std::string, double>> correlations;
    nlohmann::json decisions = nlohmann::json::array();
    nlohmann::json descriptive_statistics;
    std::size_t open_accepted_trades = 0;
};

struct PortfolioStatistics {
    std::unordered_map<std::string, double> annualized_volatility;
    std::unordered_map<std::string, std::unordered_map<std::string, double>> correlations;
    nlohmann::json evidence;
};

// Expanding estimates advance only through complete return intervals ending before
// the decision. Each interval and matched pair is visited once across a run.
class PortfolioReturnEstimator {
public:
    void add_history(const std::string& symbol, const std::vector<MarketEvent>& events) {
        if (started_ || symbol.empty() || histories_.count(symbol))
            throw std::invalid_argument("Portfolio histories require unique symbols before estimation");
        History history;
        for (std::size_t i = 0; i < events.size(); ++i) {
            if (!std::isfinite(events[i].price) || events[i].price <= 0.0)
                throw std::invalid_argument("Portfolio history requires finite positive prices: " + symbol);
            if (i == 0) continue;
            if (events[i].timestamp <= events[i - 1].timestamp)
                throw std::invalid_argument("Portfolio history requires strictly increasing timestamps: " + symbol);
            const long double seconds = std::chrono::duration<long double>(events[i].timestamp - events[i - 1].timestamp).count();
            const long double change = std::log(static_cast<long double>(events[i].price)) -
                                       std::log(static_cast<long double>(events[i - 1].price));
            history.returns.push_back({events[i - 1].timestamp, events[i].timestamp, seconds, change});
        }
        histories_.emplace(symbol, std::move(history));
    }

    PortfolioStatistics before(std::chrono::system_clock::time_point cutoff) {
        if (started_ && cutoff < last_cutoff_)
            throw std::invalid_argument("Portfolio estimator cutoffs must not move backwards");
        started_ = true;
        last_cutoff_ = cutoff;
        PortfolioStatistics result;
        result.evidence = {{"estimator", "expanding-time-aware-log-returns-v1"},
                           {"cutoff_exclusive_ms", milliseconds(cutoff)},
                           {"volatility", nlohmann::json::object()},
                           {"correlation", nlohmann::json::object()}};
        for (auto& [symbol, history] : histories_) {
            while (history.used < history.returns.size() && history.returns[history.used].end < cutoff) {
                const auto& interval = history.returns[history.used++];
                history.volatility.add(interval);
            }
            const auto& v = history.volatility;
            const bool available = v.count >= 2;
            auto window = window_json(v.count, v.start, v.end);
            window["available"] = available;
            window["observed_seconds"] = static_cast<double>(v.seconds);
            if (available) {
                // Weighted residual variance of log-return rates. For equal intervals
                // this is sample return variance multiplied by seconds/year / interval.
                const double value = static_cast<double>(std::sqrt(std::max(0.0L, v.m2) /
                    static_cast<long double>(v.count - 1) * seconds_per_year));
                if (!std::isfinite(value)) throw std::runtime_error("Non-finite portfolio volatility estimate");
                result.annualized_volatility.emplace(symbol, value);
                window["annualized_volatility"] = value;
            }
            result.evidence["volatility"][symbol] = std::move(window);
        }
        for (auto a = histories_.begin(); a != histories_.end(); ++a) {
            for (auto b = a; b != histories_.end(); ++b) {
                auto& pair = pairs_[{a->first, b->first}];
                while (pair.left < a->second.used && pair.right < b->second.used) {
                    const auto& x = a->second.returns[pair.left];
                    const auto& y = b->second.returns[pair.right];
                    const auto x_interval = std::make_pair(x.start, x.end);
                    const auto y_interval = std::make_pair(y.start, y.end);
                    if (x_interval < y_interval) { ++pair.left; continue; }
                    if (y_interval < x_interval) { ++pair.right; continue; }
                    pair.add(x, y);
                    ++pair.left;
                    ++pair.right;
                }
                const bool available = pair.count >= 2 && pair.m2_x > 0.0L && pair.m2_y > 0.0L;
                auto window = window_json(pair.count, pair.start, pair.end);
                window["available"] = available;
                if (available) {
                    const double value = static_cast<double>(std::clamp(pair.covariance /
                        std::sqrt(pair.m2_x * pair.m2_y), -1.0L, 1.0L));
                    if (!std::isfinite(value)) throw std::runtime_error("Non-finite portfolio correlation estimate");
                    result.correlations[a->first][b->first] = value;
                    result.correlations[b->first][a->first] = value;
                    window["correlation"] = value;
                }
                result.evidence["correlation"][a->first][b->first] = window;
                result.evidence["correlation"][b->first][a->first] = std::move(window);
            }
        }
        return result;
    }

    static std::int64_t milliseconds(std::chrono::system_clock::time_point value) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(value.time_since_epoch()).count();
    }

private:
    using TimePoint = std::chrono::system_clock::time_point;
    static constexpr long double seconds_per_year = 365.0L * 24.0L * 60.0L * 60.0L;
    struct ReturnInterval { TimePoint start; TimePoint end; long double seconds; long double change; };
    struct Volatility {
        std::size_t count = 0;
        long double seconds = 0.0L, mean_rate = 0.0L, m2 = 0.0L;
        TimePoint start{}, end{};
        void add(const ReturnInterval& value) {
            if (count++ == 0) start = value.start;
            end = value.end;
            seconds += value.seconds;
            const long double rate = value.change / value.seconds;
            const long double delta = rate - mean_rate;
            mean_rate += delta * value.seconds / seconds;
            m2 += value.seconds * delta * (rate - mean_rate);
        }
    };
    struct History { std::vector<ReturnInterval> returns; std::size_t used = 0; Volatility volatility; };
    struct Correlation {
        std::size_t left = 0, right = 0, count = 0;
        long double mean_x = 0.0L, mean_y = 0.0L, m2_x = 0.0L, m2_y = 0.0L, covariance = 0.0L;
        TimePoint start{}, end{};
        void add(const ReturnInterval& x, const ReturnInterval& y) {
            if (count++ == 0) start = x.start;
            end = x.end;
            const long double dx = x.change - mean_x, dy = y.change - mean_y;
            mean_x += dx / count;
            mean_y += dy / count;
            m2_x += dx * (x.change - mean_x);
            m2_y += dy * (y.change - mean_y);
            covariance += dx * (y.change - mean_y);
        }
    };
    static nlohmann::json window_json(std::size_t count, TimePoint start, TimePoint end) {
        return {{"return_intervals", count},
                {"window_start_ms", count ? nlohmann::json(milliseconds(start)) : nlohmann::json(nullptr)},
                {"window_end_ms", count ? nlohmann::json(milliseconds(end)) : nlohmann::json(nullptr)}};
    }
    std::map<std::string, History> histories_;
    std::map<std::pair<std::string, std::string>, Correlation> pairs_;
    bool started_ = false;
    TimePoint last_cutoff_{};
};

inline PortfolioResearchConfig load_portfolio_research_config(const std::string& path) {
    std::ifstream file(path);
    if (!file) throw std::runtime_error("Cannot open portfolio research config: " + path);
    nlohmann::json json; file >> json;
    PortfolioResearchConfig c;
    c.strategy = json.value("strategy", c.strategy);
    c.starting_equity = json.value("starting_equity", c.starting_equity);
    if (!std::isfinite(c.starting_equity) || !(c.starting_equity > 0.0)) throw std::runtime_error("starting_equity must be finite and positive");
    if (!json.contains("datasets") || !json.at("datasets").is_array() || json.at("datasets").empty())
        throw std::runtime_error("portfolio research requires a non-empty datasets array");
    for (const auto& item : json.at("datasets")) {
        PortfolioDataset d{item.value("path", std::string{}), item.value("symbol", std::string{}), item.value("weight", 1.0)};
        if (d.path.empty() || d.symbol.empty() || !std::isfinite(d.weight) || !(d.weight > 0.0)) throw std::runtime_error("invalid portfolio dataset");
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
        c.portfolio_risk.minimum_size_multiplier = r.value("minimum_size_multiplier", c.portfolio_risk.minimum_size_multiplier);
        c.portfolio_risk.maximum_size_multiplier = r.value("maximum_size_multiplier", c.portfolio_risk.maximum_size_multiplier);
        c.portfolio_risk.correlation_threshold = r.value("correlation_threshold", c.portfolio_risk.correlation_threshold);
    }
    return c;
}

class PortfolioResearchRunner {
public:
    explicit PortfolioResearchRunner(RiskConfig risk) : risk_(std::move(risk)) {}

    PortfolioResearchSummary run(const PortfolioResearchConfig& config) const {
        if (!std::isfinite(config.starting_equity) || config.starting_equity <= 0.0 || config.datasets.empty())
            throw std::invalid_argument("Portfolio research requires finite positive equity and datasets");
        sentum::risk::PortfolioRiskManager manager(config.portfolio_risk);
        PortfolioReturnEstimator estimator;
        struct AssetData { PortfolioDataset cfg; std::vector<TradePosition> trades; bool has_open_trade = false; };
        std::vector<AssetData> assets;
        assets.reserve(config.datasets.size());
        std::vector<TradePosition> raw_trades;
        std::vector<TradePosition> candidates;

        for (const auto& dataset : config.datasets) {
            if (!std::isfinite(dataset.weight) || dataset.weight <= 0.0)
                throw std::invalid_argument("Portfolio dataset weight must be finite and positive");
            AssetData a; a.cfg = dataset;
            const auto events = HistoricalEventReader::read_csv(dataset.path, dataset.symbol);
            if (events.size() < 3) throw std::runtime_error("portfolio dataset requires at least 3 events: " + dataset.symbol);
            estimator.add_history(dataset.symbol, events);
            auto clock = std::make_shared<ReplayClock>();
            auto strategy = sentum::strategy::StrategyFactory::create(config.strategy);
            TradeEngine engine(dataset.symbol, risk_, clock, std::move(strategy), ":memory:");
            for (const auto& event : events) { clock->advance_to(event.timestamp); engine.process_event(event); }
            a.trades = engine.completed_trades();
            for (auto trade : a.trades) {
                scale_trade(trade, dataset.weight);
                raw_trades.push_back(trade);
                candidates.push_back(std::move(trade));
            }
            // An entry's existence cannot depend on whether future data closes it.
            auto outstanding = engine.get_current_position();
            if (outstanding.open) {
                a.has_open_trade = true;
                scale_trade(outstanding, dataset.weight);
                candidates.push_back(std::move(outstanding));
            }
            assets.push_back(std::move(a));
        }

        PortfolioResearchSummary out;
        out.raw_combined = MetricsCalculator::calculate(sorted_by_exit(raw_trades));
        out.candidate_trades = candidates.size();
        for (const auto& a : assets)
            out.assets.push_back({a.cfg.symbol, MetricsCalculator::calculate(a.trades), a.trades.size() + a.has_open_trade});

        std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
            if (a.entry_time != b.entry_time) return a.entry_time < b.entry_time;
            return a.symbol < b.symbol;
        });

        double equity = config.starting_equity;
        const double day_start = equity;
        std::vector<TradePosition> open;
        std::vector<TradePosition> accepted;
        std::deque<std::chrono::system_clock::time_point> trade_times;
        std::size_t consecutive_losses = 0;

        for (auto trade : candidates) {
            close_until(trade.entry_time, open, accepted, equity, consecutive_losses);
            sentum::risk::PortfolioRiskSnapshot snapshot;
            snapshot.equity = equity;
            snapshot.day_start_equity = day_start;
            const auto statistics = estimator.before(trade.entry_time);
            snapshot.correlations = statistics.correlations;
            snapshot.consecutive_losses = consecutive_losses;
            snapshot.trade_times = trade_times;
            snapshot.annualized_volatility = statistics.annualized_volatility;
            for (const auto& p : open) snapshot.positions.push_back({p.symbol, p.entry_price * p.quantity});
            const double proposed = trade.entry_price * trade.quantity;
            const auto decision = manager.approve(trade.symbol, proposed, snapshot, trade.entry_time);
            double approved_quantity = 0.0;
            if (decision.approved) {
                approved_quantity = decision.approved_notional / trade.entry_price;
                if (!std::isfinite(approved_quantity) || approved_quantity <= 0.0)
                    throw std::runtime_error("Invalid approved portfolio quantity");
                if (trade.entry_price * approved_quantity > decision.approved_notional)
                    approved_quantity = std::nextafter(approved_quantity, 0.0);
            }
            out.decisions.push_back({{"symbol", trade.symbol},
                {"entry_time_ms", PortfolioReturnEstimator::milliseconds(trade.entry_time)},
                {"equity", equity}, {"proposed_notional", proposed},
                {"approved", decision.approved}, {"reason", decision.reason},
                {"size_multiplier", decision.size_multiplier},
                {"approved_notional", decision.approved_notional},
                {"approved_quantity", approved_quantity},
                {"accepted_notional", trade.entry_price * approved_quantity},
                {"statistics", statistics.evidence}});
            if (!decision.approved) { ++out.rejected_trades; continue; }
            scale_trade(trade, approved_quantity / trade.quantity);
            // Consume the exact final amount approved by the portfolio boundary.
            trade.quantity = approved_quantity;
            open.push_back(std::move(trade));
            trade_times.push_back(open.back().entry_time);
            while (!trade_times.empty() && trade_times.front() < open.back().entry_time - std::chrono::hours(1)) trade_times.pop_front();
            ++out.accepted_trades;
        }
        close_until(std::chrono::system_clock::time_point::max(), open, accepted, equity, consecutive_losses);
        out.open_accepted_trades = open.size();
        out.portfolio_filtered = MetricsCalculator::calculate(sorted_by_exit(accepted));
        const auto descriptive = estimator.before(std::chrono::system_clock::time_point::max());
        out.correlations = descriptive.correlations;
        out.descriptive_statistics = descriptive.evidence;
        for (auto& asset : out.assets) {
            const auto value = descriptive.annualized_volatility.find(asset.symbol);
            if (value != descriptive.annualized_volatility.end()) {
                asset.volatility = value->second;
                asset.volatility_available = true;
            }
        }
        return out;
    }

    static nlohmann::json to_json(const PortfolioResearchSummary& s) {
        auto metrics = [](const BacktestMetrics& m) { return nlohmann::json{{"trades",m.trades},{"net_profit",m.net_profit},{"max_drawdown",m.max_drawdown},{"profit_factor",std::isfinite(m.profit_factor)?m.profit_factor:0.0},{"win_rate",m.win_rate},{"expectancy",m.expectancy},{"sharpe",m.sharpe},{"sortino",m.sortino},{"fee_share",m.fee_share}}; };
        nlohmann::json j{{"candidate_trades",s.candidate_trades},{"accepted_trades",s.accepted_trades},{"rejected_trades",s.rejected_trades},{"raw_combined",metrics(s.raw_combined)},{"portfolio_filtered",metrics(s.portfolio_filtered)},{"assets",nlohmann::json::array()},{"correlations",s.correlations}};
        j["decisions"] = s.decisions;
        j["descriptive_statistics"] = s.descriptive_statistics;
        j["open_accepted_trades"] = s.open_accepted_trades;
        for (const auto& a : s.assets) j["assets"].push_back({{"symbol",a.symbol},{"candidate_trades",a.candidate_trades},{"volatility",a.volatility},{"volatility_available",a.volatility_available},{"metrics",metrics(a.raw_metrics)}});
        return j;
    }

    static void write_artifact(const PortfolioResearchSummary& summary, const std::string& path = "log/portfolio_research_latest.json") {
        const auto parent = std::filesystem::path(path).parent_path(); if (!parent.empty()) std::filesystem::create_directories(parent);
        const std::string tmp = path + ".tmp"; { std::ofstream f(tmp, std::ios::trunc); if (!f) throw std::runtime_error("cannot write portfolio research artifact"); f << to_json(summary).dump(2) << '\n'; }
        std::error_code ec; std::filesystem::remove(path, ec); ec.clear(); std::filesystem::rename(tmp, path, ec); if (ec) throw std::runtime_error("cannot publish portfolio research artifact: " + ec.message());
    }

private:
    static void scale_trade(TradePosition& t, double multiplier) {
        t.quantity *= multiplier; t.gross_profit *= multiplier; t.net_profit *= multiplier; t.fee_entry *= multiplier; t.fee_exit *= multiplier; t.capital_at_risk *= multiplier;
    }

    static std::vector<TradePosition> sorted_by_exit(std::vector<TradePosition> v) {
        std::sort(v.begin(),v.end(),[](const auto&a,const auto&b){return a.exit_time<b.exit_time;});return v;
    }

    static void close_until(std::chrono::system_clock::time_point at, std::vector<TradePosition>& open,
                            std::vector<TradePosition>& accepted, double& equity, std::size_t& losses) {
        auto it=open.begin();while(it!=open.end()){if(!it->open && it->exit_time<=at){equity+=it->net_profit;losses=it->net_profit<0?losses+1:0;accepted.push_back(*it);it=open.erase(it);}else ++it;}
    }

    RiskConfig risk_;
};

} // namespace sentum::research
