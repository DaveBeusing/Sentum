#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <deque>
#include <stdexcept>
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
    double approved_notional = 0.0;
};

class PortfolioRiskManager {
public:
    explicit PortfolioRiskManager(PortfolioRiskConfig config = {}) : config_(std::move(config)) {
        for (const double value : {config_.max_gross_exposure, config_.max_asset_exposure,
                                   config_.max_correlated_exposure, config_.max_daily_drawdown,
                                   config_.target_volatility})
            if (!std::isfinite(value) || value < 0.0)
                throw std::invalid_argument("Portfolio risk limits must be finite and nonnegative");
        if (!std::isfinite(config_.minimum_size_multiplier) || config_.minimum_size_multiplier <= 0.0 ||
            !std::isfinite(config_.maximum_size_multiplier) ||
            config_.maximum_size_multiplier < config_.minimum_size_multiplier ||
            !std::isfinite(config_.correlation_threshold) || config_.correlation_threshold < 0.0 ||
            config_.correlation_threshold > 1.0)
            throw std::invalid_argument("Invalid portfolio sizing or correlation bounds");
    }

    PortfolioDecision approve(const std::string& symbol, double proposed_notional,
                              const PortfolioRiskSnapshot& snapshot,
                              std::chrono::system_clock::time_point now) const {
        if (symbol.empty() || !std::isfinite(snapshot.equity) || !(snapshot.equity > 0.0) ||
            !std::isfinite(proposed_notional) || !(proposed_notional > 0.0) ||
            !std::isfinite(snapshot.day_start_equity) || snapshot.day_start_equity < 0.0)
            return {false, 0.0, "invalid portfolio equity or proposed notional"};

        double multiplier = 1.0;
        const auto vol = snapshot.annualized_volatility.find(symbol);
        if (vol != snapshot.annualized_volatility.end()) {
            if (!std::isfinite(vol->second) || vol->second < 0.0)
                return {false, 0.0, "invalid portfolio volatility"};
            if (vol->second > 0.0 && config_.target_volatility > 0.0) {
                // Divide in extended precision so finite inputs cannot overflow before clamping.
                const long double ratio = static_cast<long double>(config_.target_volatility) / vol->second;
                multiplier = static_cast<double>(std::clamp(ratio,
                    static_cast<long double>(config_.minimum_size_multiplier),
                    static_cast<long double>(config_.maximum_size_multiplier)));
            }
        }
        multiplier = std::clamp(multiplier, config_.minimum_size_multiplier, config_.maximum_size_multiplier);
        const double final_notional = proposed_notional * multiplier;
        if (!std::isfinite(final_notional) || !(final_notional > 0.0))
            return {false, 0.0, "invalid scaled portfolio notional"};

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

        long double gross = 0.0, same_asset = 0.0, correlated = 0.0;
        for (const auto& position : snapshot.positions) {
            if (position.symbol.empty() || !std::isfinite(position.notional))
                return {false, 0.0, "invalid portfolio position"};
            const long double notional = std::abs(static_cast<long double>(position.notional));
            gross += notional;
            if (position.symbol == symbol) same_asset += notional;
            // Unknown correlation is not evidence of diversification. Same-asset exposure
            // is always correlated, even before an estimator has enough observations.
            bool is_correlated = true;
            const auto row = snapshot.correlations.find(symbol);
            if (row != snapshot.correlations.end()) {
                const auto corr = row->second.find(position.symbol);
                if (corr != row->second.end()) {
                    if (!std::isfinite(corr->second) || std::abs(corr->second) > 1.0)
                        return {false, 0.0, "invalid portfolio correlation"};
                    is_correlated = position.symbol == symbol ||
                                    std::abs(corr->second) >= config_.correlation_threshold;
                }
            }
            if (is_correlated) correlated += notional;
        }

        const auto limit_notional = [&](double fraction) {
            const double amount = snapshot.equity * fraction;
            return std::isfinite(amount) ? static_cast<long double>(amount)
                                        : static_cast<long double>(snapshot.equity) * fraction;
        };
        if (gross + final_notional > limit_notional(config_.max_gross_exposure))
            return {false, 0.0, "gross exposure limit exceeded"};
        if (same_asset + final_notional > limit_notional(config_.max_asset_exposure))
            return {false, 0.0, "asset exposure limit exceeded"};
        if (correlated + final_notional > limit_notional(config_.max_correlated_exposure))
            return {false, 0.0, "correlated exposure limit exceeded"};
        return {true, multiplier, "portfolio risk approved", final_notional};
    }

    const PortfolioRiskConfig& config() const noexcept { return config_; }

private:
    PortfolioRiskConfig config_;
};

} // namespace sentum::risk
