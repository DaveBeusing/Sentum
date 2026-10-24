#include <cmath>
#include <chrono>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>
#include <sentum/trader/utils/RiskConfigLoader.hpp>

namespace {
double required_number(const nlohmann::json& j, const char* key) {
    if (!j.contains(key) || !j.at(key).is_number()) throw std::runtime_error(std::string("Missing or invalid risk key: ") + key);
    const auto value = j.at(key).get<double>();
    if (!std::isfinite(value)) throw std::runtime_error(std::string("Non-finite risk key: ") + key);
    return value;
}
std::int64_t required_integer(const nlohmann::json& j, const char* key) {
    if (!j.contains(key) || !j.at(key).is_number_integer()) throw std::runtime_error(std::string("Missing or invalid risk key: ") + key);
    if (j.at(key).is_number_unsigned() &&
        j.at(key).get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        throw std::runtime_error(std::string("Overflowing risk key: ") + key);
    return j.at(key).get<std::int64_t>();
}
}

void validate_stop_loss_config(const RiskConfig& config) {
    if (!std::isfinite(config.stop_loss_percent) ||
        config.stop_loss_percent <= 0.0 || config.stop_loss_percent >= 1.0) {
        throw std::invalid_argument("stop_loss_percent must be finite and strictly between zero and one");
    }
    if (!std::isfinite(config.trailing_sl_percent) ||
        config.trailing_sl_percent < 0.0 || config.trailing_sl_percent >= 1.0 ||
        (config.trailing_sl_enabled && config.trailing_sl_percent == 0.0)) {
        throw std::invalid_argument(
            "trailing_sl_percent must be finite, below one, and positive when trailing stop loss is enabled");
    }
}

void validate_risk_config(const RiskConfig& c) {
    validate_stop_loss_config(c);
    const auto positive = [](double value) { return std::isfinite(value) && value > 0.0; };
    const auto fraction = [](double value) { return std::isfinite(value) && value >= 0.0 && value < 1.0; };
    if (!positive(c.max_total_capital) || !positive(c.risk_per_trade) || c.risk_per_trade > 1.0 ||
        !positive(c.max_total_capital * c.risk_per_trade))
        throw std::invalid_argument("Capital and risk_per_trade must define a finite positive risk budget");
    if (!positive(c.take_profit_percent) || !std::isfinite(1.0 + c.take_profit_percent))
        throw std::invalid_argument("take_profit_percent must be finite and positive");
    if (!fraction(c.buy_fee_percent) || !fraction(c.sell_fee_percent) || !fraction(c.slippage_percent) ||
        !std::isfinite(c.spread_percent) || c.spread_percent < 0.0 || c.spread_percent >= 2.0)
        throw std::invalid_argument("Execution costs must be finite and preserve a positive executable price");
    if (!fraction(c.trailing_tp_percent) || (c.trailing_tp_enabled && c.trailing_tp_percent == 0.0))
        throw std::invalid_argument("trailing_tp_percent must be below one and positive when enabled");
    if (!positive(c.leverage) || !positive(c.max_total_capital * c.leverage) ||
        !positive(c.step_size) || !positive(c.min_quantity) || !positive(c.min_notional) ||
        !std::isfinite(c.max_quantity) || c.max_quantity < 0.0)
        throw std::invalid_argument("Capital limits and quantity filters must be finite and valid");
    if (c.max_quantity > 0.0 && c.max_quantity < c.min_quantity)
        throw std::invalid_argument("max_quantity must be zero or at least min_quantity");
    const auto maximum_seconds = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::duration::max()).count();
    const auto maximum_milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::duration::max()).count();
    if (c.cooldown_seconds < 0 || c.cooldown_seconds > maximum_seconds ||
        c.max_holding_seconds <= 0 || c.max_holding_seconds > maximum_seconds ||
        c.max_data_age_ms <= 0 || c.max_data_age_ms > maximum_milliseconds)
        throw std::invalid_argument("Risk time constraints must fit the execution clock");
}

RiskConfig load_risk_config(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) throw std::runtime_error("Cannot open risk configuration: " + path);
    nlohmann::json j;
    file >> j;
    return parse_risk_config(j);
}

RiskConfig parse_risk_config(const nlohmann::json& j) {
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

    validate_risk_config(c);
    return c;
}
