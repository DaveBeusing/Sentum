#include <fstream>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>
#include <sentum/trader/utils/RiskConfigLoader.hpp>

namespace {
double required_number(const nlohmann::json& j, const char* key) {
    if (!j.contains(key) || !j.at(key).is_number()) throw std::runtime_error(std::string("Missing or invalid risk key: ") + key);
    return j.at(key).get<double>();
}
std::int64_t required_integer(const nlohmann::json& j, const char* key) {
    if (!j.contains(key) || !j.at(key).is_number_integer()) throw std::runtime_error(std::string("Missing or invalid risk key: ") + key);
    return j.at(key).get<std::int64_t>();
}
}

RiskConfig load_risk_config(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) throw std::runtime_error("Cannot open risk configuration: " + path);
    nlohmann::json j;
    file >> j;

    RiskConfig c;
    c.max_total_capital = required_number(j, "max_total_capital");
    c.risk_per_trade = required_number(j, "risk_per_trade");
    c.stop_loss_percent = required_number(j, "stop_loss_percent");
    c.take_profit_percent = required_number(j, "take_profit_percent");
    c.buy_fee_percent = required_number(j, "buy_fee_percent");
    c.sell_fee_percent = required_number(j, "sell_fee_percent");
    c.slippage_percent = required_number(j, "slippage_percent");
    c.spread_percent = required_number(j, "spread_percent");
    c.leverage = required_number(j, "leverage");
    c.min_quantity = required_number(j, "min_quantity");
    c.max_quantity = required_number(j, "max_quantity");
    c.step_size = required_number(j, "step_size");
    c.min_notional = required_number(j, "min_notional");
    c.cooldown_seconds = required_integer(j, "cooldown_seconds");
    c.max_holding_seconds = required_integer(j, "max_holding_seconds");
    c.max_data_age_ms = required_integer(j, "max_data_age_ms");
    c.trailing_sl_enabled = j.value("trailing_sl_enabled", false);
    c.trailing_sl_percent = j.value("trailing_sl_percent", 0.0);
    c.trailing_tp_enabled = j.value("trailing_tp_enabled", false);
    c.trailing_tp_percent = j.value("trailing_tp_percent", 0.0);

    if (c.max_total_capital <= 0.0 || c.risk_per_trade <= 0.0 || c.risk_per_trade > 1.0) throw std::runtime_error("Invalid capital or risk_per_trade");
    if (c.stop_loss_percent <= 0.0 || c.take_profit_percent <= 0.0) throw std::runtime_error("Stop loss and take profit must be positive");
    if (c.buy_fee_percent < 0.0 || c.sell_fee_percent < 0.0 || c.slippage_percent < 0.0 || c.spread_percent < 0.0) throw std::runtime_error("Execution costs cannot be negative");
    if (c.leverage <= 0.0 || c.step_size <= 0.0 || c.min_quantity <= 0.0 || c.min_notional <= 0.0) throw std::runtime_error("Invalid exchange filters");
    if (c.max_quantity > 0.0 && c.max_quantity < c.min_quantity) throw std::runtime_error("max_quantity must be zero or >= min_quantity");
    if (c.cooldown_seconds < 0 || c.max_holding_seconds <= 0 || c.max_data_age_ms <= 0) throw std::runtime_error("Invalid time constraints");
    return c;
}
