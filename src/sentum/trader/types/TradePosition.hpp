#pragma once

#include <chrono>
#include <cstdint>
#include <string>

struct TradePosition {
    bool open = false;
    bool simulated = false;
    bool risk_approved = false;
    std::string symbol;
    std::string source;
    std::string strategy;
    std::string signal_reason;
    std::string risk_reason;
    std::string notes;
    std::string order_type;
    std::string client_order_id;
    std::string exchange_order_id;
    double reference_price = 0.0;
    double entry_price = 0.0;
    double quantity = 0.0;
    double executed_price = 0.0;
    std::int64_t execution_latency_ms = 0;
    std::chrono::system_clock::time_point signal_time{};
    std::chrono::system_clock::time_point entry_time{};
    double highest_price = 0.0;
    double lowest_price = 0.0;
    double stop_loss_price = 0.0;
    double take_profit_price = 0.0;
    double exit_price = 0.0;
    std::chrono::system_clock::time_point exit_time{};
    double gross_profit = 0.0;
    double net_profit = 0.0;
    double fee_entry = 0.0;
    double fee_exit = 0.0;
    double initial_balance = 0.0;
    double closing_balance = 0.0;
    double capital_at_risk = 0.0;
    double leverage = 1.0;
    double risk_per_trade = 0.0;
    double stop_loss_percent = 0.0;
    bool trailing_sl_enabled = false;
    double trailing_sl_percent = 0.0;
    double take_profit_percent = 0.0;
    bool trailing_tp_enabled = false;
    double trailing_tp_percent = 0.0;
    double buy_fee_percent = 0.0;
    double sell_fee_percent = 0.0;
    std::string close_reason;
    bool stop_loss_triggered = false;
    bool take_profit_triggered = false;

    double holding_seconds() const {
        const auto end = open ? std::chrono::system_clock::now() : exit_time;
        if (entry_time == std::chrono::system_clock::time_point{}) return 0.0;
        return std::chrono::duration<double>(end - entry_time).count();
    }
};
