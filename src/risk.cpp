#include "../include/risk.hpp"

double calculate_quantity(double usdt_balance, double price, const RiskConfig& config) {
    double max_position = usdt_balance * config.position_percent;
    if (max_position > config.max_total_position_usdt)
        max_position = config.max_total_position_usdt;
    return max_position / price;
}

bool should_stop_loss(double entry_price, double current_price, const RiskConfig& config) {
    return current_price <= (entry_price * (1.0 - config.stop_loss_percent));
}
