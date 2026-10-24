#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

#include <sentum/time/Clock.hpp>
#include <sentum/trader/strategy/IStrategy.hpp>
#include <sentum/trader/types/RiskConfig.hpp>
#include <sentum/trader/utils/RiskConfigLoader.hpp>

struct RiskDecision {
    bool approved = false;
    std::string reason;
    double quantity = 0.0;
    double notional = 0.0;
};

class RiskManager {
public:
    explicit RiskManager(const RiskConfig& config,
                         std::shared_ptr<IClock> clock = std::make_shared<SystemClock>())
        : config_(config), clock_(std::move(clock)) {
        if (!clock_) throw std::invalid_argument("Risk manager requires a clock");
        validate_risk_config(config_);
    }

    RiskDecision approve_entry(const StrategySignal& signal, double price,
        std::chrono::system_clock::time_point price_time,
        std::chrono::system_clock::time_point last_exit) const {
        const auto now = clock_->now();
        if (signal.action != TradeAction::BUY) return {false, "signal is not BUY"};
        if (!std::isfinite(price) || price <= 0.0) return {false, "invalid market price"};
        if (now >= price_time && elapsed_ticks(now, price_time) > ticks(std::chrono::milliseconds(config_.max_data_age_ms)))
            return {false, "market data is stale"};
        if (last_exit != std::chrono::system_clock::time_point{} &&
            (now < last_exit || elapsed_ticks(now, last_exit) < ticks(std::chrono::seconds(config_.cooldown_seconds))))
            return {false, "cooldown active"};

        const double candidate_fill = price * (1.0 + config_.spread_percent * 0.5) *
                                      (1.0 + config_.slippage_percent);
        const double candidate_stop = candidate_fill * (1.0 - config_.stop_loss_percent);
        const double candidate_target = candidate_fill * (1.0 + config_.take_profit_percent);
        if (!std::isfinite(candidate_fill) || candidate_fill <= 0.0 ||
            !std::isfinite(candidate_stop) || candidate_stop <= 0.0 ||
            !std::isfinite(candidate_target) || candidate_target <= 0.0)
            return {false, "invalid executable position levels"};

        const double capital_at_risk = config_.max_total_capital * config_.risk_per_trade;
        const double loss_per_unit = price * config_.stop_loss_percent;
        if (!std::isfinite(loss_per_unit) || loss_per_unit <= 0.0) return {false, "invalid stop-loss distance"};
        double quantity = capital_at_risk / loss_per_unit;
        if (!std::isfinite(quantity) || quantity <= 0.0 || !std::isfinite(quantity / config_.step_size))
            return {false, "invalid position quantity"};
        const double approved_limit = quantity;
        quantity = std::floor(quantity / config_.step_size) * config_.step_size;
        if (quantity > approved_limit) quantity = std::floor(std::nextafter(quantity / config_.step_size, 0.0)) * config_.step_size;
        const double notional = quantity * price;
        if (!std::isfinite(quantity) || quantity <= 0.0 || quantity > approved_limit ||
            !std::isfinite(notional) || notional <= 0.0 ||
            !std::isfinite(quantity * candidate_fill) || quantity * candidate_fill <= 0.0 ||
            !std::isfinite(quantity * candidate_target) || quantity * candidate_target <= 0.0)
            return {false, "invalid position notional"};
        if (quantity < config_.min_quantity) return {false, "quantity below exchange minimum"};
        if (config_.max_quantity > 0.0 && quantity > config_.max_quantity) return {false, "quantity above exchange maximum"};
        if (notional < config_.min_notional) return {false, "notional below exchange minimum"};
        if (notional > config_.max_total_capital * config_.leverage) return {false, "position exceeds configured capital"};
        return {true, "approved", quantity, notional};
    }

private:
    using TickCount = std::make_unsigned_t<std::chrono::system_clock::duration::rep>;
    static TickCount elapsed_ticks(std::chrono::system_clock::time_point later,
                                   std::chrono::system_clock::time_point earlier) noexcept {
        // The caller establishes ordering. Unsigned subtraction preserves the
        // full positive difference even when signed clock subtraction overflows.
        return static_cast<TickCount>(later.time_since_epoch().count()) -
               static_cast<TickCount>(earlier.time_since_epoch().count());
    }
    template<class Duration> static TickCount ticks(Duration duration) noexcept {
        return static_cast<TickCount>(std::chrono::duration_cast<std::chrono::system_clock::duration>(duration).count());
    }
    RiskConfig config_;
    std::shared_ptr<IClock> clock_;
};
