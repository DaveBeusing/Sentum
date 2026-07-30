#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

#include <sentum/trader/strategy/IStrategy.hpp>
#include <sentum/trader/types/RiskConfig.hpp>

struct RiskDecision {
    bool approved = false;
    std::string reason;
    double quantity = 0.0;
    double notional = 0.0;
};

class RiskManager {
public:
    explicit RiskManager(const RiskConfig& config) : config_(config) {}

    RiskDecision approve_entry(const StrategySignal& signal, double price,
        std::chrono::system_clock::time_point price_time,
        std::chrono::system_clock::time_point last_exit) const {
        const auto now = std::chrono::system_clock::now();
        if (signal.action != TradeAction::BUY) return {false, "signal is not BUY"};
        if (price <= 0.0) return {false, "invalid market price"};
        if (now - price_time > std::chrono::milliseconds(config_.max_data_age_ms)) return {false, "market data is stale"};
        if (last_exit != std::chrono::system_clock::time_point{} &&
            now - last_exit < std::chrono::seconds(config_.cooldown_seconds)) return {false, "cooldown active"};

        const double capital_at_risk = config_.max_total_capital * config_.risk_per_trade;
        const double loss_per_unit = price * config_.stop_loss_percent;
        if (loss_per_unit <= 0.0) return {false, "invalid stop-loss distance"};
        double quantity = capital_at_risk / loss_per_unit;
        quantity = std::floor(quantity / config_.step_size) * config_.step_size;
        const double notional = quantity * price;
        if (quantity < config_.min_quantity) return {false, "quantity below exchange minimum"};
        if (config_.max_quantity > 0.0 && quantity > config_.max_quantity) return {false, "quantity above exchange maximum"};
        if (notional < config_.min_notional) return {false, "notional below exchange minimum"};
        if (notional > config_.max_total_capital * config_.leverage) return {false, "position exceeds configured capital"};
        return {true, "approved", quantity, notional};
    }

private:
    RiskConfig config_;
};
