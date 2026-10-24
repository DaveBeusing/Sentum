#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>
#include <sentum/trader/types/RiskConfig.hpp>
#include <sentum/trader/utils/RiskConfigLoader.hpp>

namespace sentum::risk {

inline nlohmann::json position_policy_identity(const RiskConfig& risk) {
    return {{"max_total_capital", risk.max_total_capital}, {"risk_per_trade", risk.risk_per_trade},
        {"stop_loss_percent", risk.stop_loss_percent}, {"take_profit_percent", risk.take_profit_percent},
        {"trailing_sl_enabled", risk.trailing_sl_enabled}, {"trailing_sl_percent", risk.trailing_sl_percent},
        {"trailing_tp_enabled", risk.trailing_tp_enabled}, {"trailing_tp_percent", risk.trailing_tp_percent},
        {"buy_fee_percent", risk.buy_fee_percent}, {"sell_fee_percent", risk.sell_fee_percent},
        {"slippage_percent", risk.slippage_percent}, {"spread_percent", risk.spread_percent},
        {"leverage", risk.leverage}, {"min_quantity", risk.min_quantity}, {"max_quantity", risk.max_quantity},
        {"step_size", risk.step_size}, {"min_notional", risk.min_notional},
        {"cooldown_seconds", risk.cooldown_seconds}, {"max_holding_seconds", risk.max_holding_seconds},
        {"max_data_age_ms", risk.max_data_age_ms}};
}

struct PositionPolicyCheckpoint {
    std::string environment = "binance-spot-testnet";
    std::string symbol;
    std::string entry_client_order_id;
    std::int64_t entry_exchange_order_id = 0;
    std::int64_t entry_time_ms = 0;
    nlohmann::json risk_policy;
    double quantity = 0.0;
    double entry_price = 0.0;
    double highest_price = 0.0;
    double lowest_price = 0.0;
    double stop_loss_price = 0.0;
    double take_profit_price = 0.0;
    std::int64_t updated_at_ms = 0;

    nlohmann::json to_json() const {
        return {{"format", 1}, {"environment", environment}, {"symbol", symbol},
            {"entry_client_order_id", entry_client_order_id}, {"entry_exchange_order_id", entry_exchange_order_id},
            {"entry_time_ms", entry_time_ms}, {"risk_policy", risk_policy}, {"quantity", quantity},
            {"entry_price", entry_price}, {"highest_price", highest_price}, {"lowest_price", lowest_price},
            {"stop_loss_price", stop_loss_price}, {"take_profit_price", take_profit_price},
            {"updated_at_ms", updated_at_ms}};
    }

    static PositionPolicyCheckpoint from_json(const nlohmann::json& value) {
        if (!value.is_object() || value.size() != 14 || integer(value, "format") != 1)
            throw std::runtime_error("Unsupported position policy checkpoint");
        PositionPolicyCheckpoint result;
        result.environment = text(value, "environment");
        result.symbol = text(value, "symbol");
        result.entry_client_order_id = text(value, "entry_client_order_id");
        result.entry_exchange_order_id = integer(value, "entry_exchange_order_id");
        result.entry_time_ms = integer(value, "entry_time_ms");
        result.risk_policy = value.at("risk_policy");
        result.quantity = positive(value, "quantity");
        result.entry_price = positive(value, "entry_price");
        result.highest_price = positive(value, "highest_price");
        result.lowest_price = positive(value, "lowest_price");
        result.stop_loss_price = positive(value, "stop_loss_price");
        result.take_profit_price = positive(value, "take_profit_price");
        result.updated_at_ms = integer(value, "updated_at_ms");
        result.validate();
        return result;
    }

    bool same_position(const PositionPolicyCheckpoint& other) const noexcept {
        return environment == other.environment && symbol == other.symbol &&
            entry_client_order_id == other.entry_client_order_id &&
            entry_exchange_order_id == other.entry_exchange_order_id;
    }

    void validate() const {
        const auto max_time = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::duration::max()).count();
        if (environment != "binance-spot-testnet" || !identifier(symbol) || !identifier(entry_client_order_id) ||
            entry_exchange_order_id <= 0 || entry_time_ms <= 0 || entry_time_ms > max_time ||
            updated_at_ms <= 0 || updated_at_ms > max_time ||
            !finite_positive(quantity) || !finite_positive(entry_price) || !finite_positive(highest_price) ||
            !finite_positive(lowest_price) || !finite_positive(stop_loss_price) || !finite_positive(take_profit_price) ||
            highest_price < entry_price || lowest_price > entry_price || lowest_price > highest_price ||
            stop_loss_price > highest_price)
            throw std::runtime_error("Invalid position policy checkpoint evidence");
        const auto expected = position_policy_identity(RiskConfig{});
        if (!risk_policy.is_object() || risk_policy.size() != expected.size())
            throw std::runtime_error("Incomplete position risk policy identity");
        for (const auto& field : expected.items()) {
            const auto& actual = risk_policy.at(field.key());
            if ((field.value().is_boolean() && !actual.is_boolean()) ||
                (field.value().is_number_integer() && !actual.is_number_integer()) ||
                (field.value().is_number_float() && (!actual.is_number() || !std::isfinite(actual.get<double>()))))
                throw std::runtime_error("Invalid position risk policy field");
        }
        RiskConfig config;
        config.max_total_capital = risk_policy.at("max_total_capital").get<double>();
        config.risk_per_trade = risk_policy.at("risk_per_trade").get<double>();
        config.stop_loss_percent = risk_policy.at("stop_loss_percent").get<double>();
        config.take_profit_percent = risk_policy.at("take_profit_percent").get<double>();
        config.trailing_sl_enabled = risk_policy.at("trailing_sl_enabled").get<bool>();
        config.trailing_sl_percent = risk_policy.at("trailing_sl_percent").get<double>();
        config.trailing_tp_enabled = risk_policy.at("trailing_tp_enabled").get<bool>();
        config.trailing_tp_percent = risk_policy.at("trailing_tp_percent").get<double>();
        config.buy_fee_percent = risk_policy.at("buy_fee_percent").get<double>();
        config.sell_fee_percent = risk_policy.at("sell_fee_percent").get<double>();
        config.slippage_percent = risk_policy.at("slippage_percent").get<double>();
        config.spread_percent = risk_policy.at("spread_percent").get<double>();
        config.leverage = risk_policy.at("leverage").get<double>();
        config.min_quantity = risk_policy.at("min_quantity").get<double>();
        config.max_quantity = risk_policy.at("max_quantity").get<double>();
        config.step_size = risk_policy.at("step_size").get<double>();
        config.min_notional = risk_policy.at("min_notional").get<double>();
        config.cooldown_seconds = integer(risk_policy, "cooldown_seconds");
        config.max_holding_seconds = integer(risk_policy, "max_holding_seconds");
        config.max_data_age_ms = integer(risk_policy, "max_data_age_ms");
        validate_risk_config(config);
    }

private:
    static bool finite_positive(double value) { return std::isfinite(value) && value > 0.0; }
    static bool identifier(const std::string& value) {
        return !value.empty() && value.size() <= 128 && std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                c == '_' || c == '-' || c == '.' || c == ':';
        });
    }
    static std::string text(const nlohmann::json& value, const char* key) {
        if (!value.at(key).is_string()) throw std::runtime_error("Invalid position policy identity");
        return value.at(key).get<std::string>();
    }
    static std::int64_t integer(const nlohmann::json& value, const char* key) {
        const auto& field = value.at(key);
        if (!field.is_number_integer() ||
            (field.is_number_unsigned() && field.get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())))
            throw std::runtime_error("Invalid position policy integer");
        return field.get<std::int64_t>();
    }
    static double positive(const nlohmann::json& value, const char* key) {
        const auto& field = value.at(key);
        if (!field.is_number() || !finite_positive(field.get<double>()))
            throw std::runtime_error("Invalid position policy level");
        return field.get<double>();
    }
};

} // namespace sentum::risk
