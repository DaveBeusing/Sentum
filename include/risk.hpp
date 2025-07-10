#pragma once

struct RiskConfig {
    double max_total_position_usdt;
    double position_percent;
    double stop_loss_percent;
};

double calculate_quantity(double usdt_balance, double price, const RiskConfig& config);
bool should_stop_loss(double entry_price, double current_price, const RiskConfig& config);
