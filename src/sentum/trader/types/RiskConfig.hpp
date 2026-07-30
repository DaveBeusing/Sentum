#pragma once

#include <cstdint>

struct RiskConfig {
    double max_total_capital = 1000.0;
    double risk_per_trade = 0.01;
    double stop_loss_percent = 0.02;
    double take_profit_percent = 0.04;
    bool trailing_sl_enabled = false;
    double trailing_sl_percent = 0.01;
    bool trailing_tp_enabled = false;
    double trailing_tp_percent = 0.02;
    double buy_fee_percent = 0.001;
    double sell_fee_percent = 0.001;
    double slippage_percent = 0.0005;
    double spread_percent = 0.0002;
    double leverage = 1.0;
    double min_quantity = 0.000001;
    double max_quantity = 0.0;
    double step_size = 0.000001;
    double min_notional = 5.0;
    std::int64_t cooldown_seconds = 30;
    std::int64_t max_holding_seconds = 900;
    std::int64_t max_data_age_ms = 2000;
};
