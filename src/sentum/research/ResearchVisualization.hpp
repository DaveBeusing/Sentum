#pragma once

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <sentum/backtest/Backtest.hpp>
#include <sentum/research/ResearchPlatform.hpp>
#include <sentum/time/Clock.hpp>
#include <sentum/trader/TradeEngine.hpp>
#include <sentum/trader/strategy/MomentumStrategy.hpp>
#include <sentum/trader/types/RiskConfig.hpp>

namespace sentum::research {

inline nlohmann::json build_research_visualization(const ResearchConfig& config,
                                                    const ResearchSummary& summary,
                                                    RiskConfig risk) {
    nlohmann::json out = {
        {"symbol", summary.symbol},
        {"objective", summary.objective},
        {"holdout_evaluated", summary.holdout_evaluated},
        {"equity_curve", nlohmann::json::array()},
        {"drawdown_curve", nlohmann::json::array()}
    };
    if (!summary.holdout_evaluated) return out;

    const auto events = HistoricalEventReader::read_csv(config.dataset, config.symbol);
    if (events.empty()) return out;
    const std::size_t holdout_n = std::max<std::size_t>(1, static_cast<std::size_t>(events.size() * config.holdout_fraction));
    const std::size_t begin = events.size() - holdout_n;

    const auto& p = summary.selected_parameters;
    risk.stop_loss_percent = p.stop_loss_percent;
    risk.take_profit_percent = p.take_profit_percent;
    risk.slippage_percent = p.slippage_percent;
    auto strategy = std::make_unique<MomentumStrategy>(p.lookback, p.entry_threshold);
    if (begin > 0) {
        const std::size_t n = std::min<std::size_t>(begin, p.lookback + 1);
        for (std::size_t i = begin - n; i < begin; ++i)
            strategy->on_price(events[i].price, events[i].timestamp);
    }

    auto clock = std::make_shared<ReplayClock>();
    TradeEngine engine(config.symbol, risk, clock, std::move(strategy), ":memory:");
    for (std::size_t i = begin; i < events.size(); ++i) {
        clock->advance_to(events[i].timestamp);
        engine.process_event(events[i]);
    }

    double equity = 0.0;
    double peak = 0.0;
    out["equity_curve"].push_back({{"ts", std::chrono::duration_cast<std::chrono::milliseconds>(events[begin].timestamp.time_since_epoch()).count()}, {"equity", 0.0}});
    for (const auto& trade : engine.completed_trades()) {
        equity += trade.net_profit;
        peak = std::max(peak, equity);
        const double drawdown = peak - equity;
        const auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(trade.exit_time.time_since_epoch()).count();
        out["equity_curve"].push_back({{"ts", ts}, {"equity", equity}});
        out["drawdown_curve"].push_back({{"ts", ts}, {"drawdown", drawdown}});
    }
    out["trades"] = engine.completed_trades().size();
    out["net_profit"] = equity;
    return out;
}

inline void write_research_visualization(const nlohmann::json& value, const std::string& path) {
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent);
    const std::string tmp = path + ".tmp";
    { std::ofstream out(tmp, std::ios::trunc); if (!out) throw std::runtime_error("Cannot write research visualization artifact"); out << value.dump(2) << '\n'; }
    std::error_code ec; std::filesystem::remove(path, ec); ec.clear(); std::filesystem::rename(tmp, path, ec);
    if (ec) throw std::runtime_error("Cannot publish research visualization artifact: " + ec.message());
}

} // namespace sentum::research
