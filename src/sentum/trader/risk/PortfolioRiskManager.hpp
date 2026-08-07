#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <deque>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace sentum::risk {

struct PortfolioRiskConfig {
    double max_gross_exposure = 1.0;
    double max_asset_exposure = 0.35;
    double max_correlated_exposure = 0.60;
    double max_daily_drawdown = 0.05;
    std::size_t max_consecutive_losses = 5;
    std::size_t max_trades_per_hour = 20;
    double target_volatility = 0.02;
    double minimum_size_multiplier = 0.20;
    double maximum_size_multiplier = 1.50;
    double correlation_threshold = 0.75;
};

struct PortfolioPosition {
    std::string symbol;
    double notional = 0.0;
};

struct PortfolioRiskSnapshot {
    double equity = 0.0;
    double day_start_equity = 0.0;
    std::vector<PortfolioPosition> positions;
    std::unordered_map<std::string, double> annualized_volatility;
    std::unordered_map<std::string, std::unordered_map<std::string, double>> correlations;
    std::size_t consecutive_losses = 0;
    std::deque<std::chrono::system_clock::time_point> trade_times;
};

struct PortfolioDecision {
    bool approved = false;
    double size_multiplier = 0.0;
    std::string reason;
};

class PortfolioRiskManager {
public:
    explicit PortfolioRiskManager(PortfolioRiskConfig config = {}) : config_(std::move(config)) {}

    PortfolioDecision approve(const std::string& symbol, double proposed_notional,
                              const PortfolioRiskSnapshot& snapshot,
                              std::chrono::system_clock::time_point now) const {
        if (!(snapshot.equity > 0.0) || !(proposed_notional > 0.0))
            return {false, 0.0, "invalid portfolio equity or proposed notional"};

        if (snapshot.day_start_equity > 0.0) {
            const double drawdown = std::max(0.0, (snapshot.day_start_equity - snapshot.equity) / snapshot.day_start_equity);
            if (drawdown >= config_.max_daily_drawdown)
                return {false, 0.0, "daily drawdown limit reached"};
        }
        if (snapshot.consecutive_losses >= config_.max_consecutive_losses)
            return {false, 0.0, "consecutive loss limit reached"};

        std::size_t trades_last_hour = 0;
        const auto cutoff = now - std::chrono::hours(1);
        for (const auto& time : snapshot.trade_times) if (time >= cutoff) ++trades_last_hour;
        if (trades_last_hour >= config_.max_trades_per_hour)
            return {false, 0.0, "hourly trade-rate limit reached"};

        double gross = 0.0, same_asset = 0.0, correlated = 0.0;
        for (const auto& position : snapshot.positions) {
            const double normalized = std::abs(position.notional) / snapshot.equity;
            gross += normalized;
            if (position.symbol == symbol) same_asset += normalized;
            const auto row = snapshot.correlations.find(symbol);
            if (row != snapshot.correlations.end()) {
                const auto corr = row->second.find(position.symbol);
                if (corr != row->second.end() && std::abs(corr->second) >= config_.correlation_threshold)
                    correlated += normalized;
            }
        }

        const double proposed_fraction = proposed_notional / snapshot.equity;
        if (gross + proposed_fraction > config_.max_gross_exposure)
            return {false, 0.0, "gross exposure limit exceeded"};
        if (same_asset + proposed_fraction > config_.max_asset_exposure)
            return {false, 0.0, "asset exposure limit exceeded"};
        if (correlated + proposed_fraction > config_.max_correlated_exposure)
            return {false, 0.0, "correlated exposure limit exceeded"};

        double multiplier = 1.0;
        const auto vol = snapshot.annualized_volatility.find(symbol);
        if (vol != snapshot.annualized_volatility.end() && vol->second > 0.0 && config_.target_volatility > 0.0)
            multiplier = config_.target_volatility / vol->second;
        multiplier = std::clamp(multiplier, config_.minimum_size_multiplier, config_.maximum_size_multiplier);
        return {true, multiplier, "portfolio risk approved"};
    }

    const PortfolioRiskConfig& config() const noexcept { return config_; }

private:
    PortfolioRiskConfig config_;
};

} // namespace sentum::risk
