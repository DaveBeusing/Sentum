#pragma once

#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace sentum::order {

enum class Side { Buy, Sell };
enum class State { Pending, Acknowledged, PartiallyFilled, Filled, Cancelling, Cancelled, Rejected, Unknown };

inline const char* to_string(State state) {
    switch (state) {
        case State::Pending: return "pending";
        case State::Acknowledged: return "acknowledged";
        case State::PartiallyFilled: return "partially_filled";
        case State::Filled: return "filled";
        case State::Cancelling: return "cancelling";
        case State::Cancelled: return "cancelled";
        case State::Rejected: return "rejected";
        case State::Unknown: return "unknown";
    }
    return "unknown";
}

struct Request {
    std::string symbol;
    Side side = Side::Buy;
    double quantity = 0.0;
    std::string client_order_id;
    std::string base_asset;
    std::string quote_asset;
    // Canonical exchange quantity, when validated against exact decimal filters.
    std::string quantity_text;
};

struct ExecutionCommission {
    std::string asset;
    double amount = 0.0;
};

struct Snapshot {
    std::string symbol;
    std::string client_order_id;
    std::int64_t exchange_order_id = 0;
    Side side = Side::Buy;
    State state = State::Pending;
    double requested_quantity = 0.0;
    double executed_quantity = 0.0;
    double cumulative_quote_quantity = 0.0;
    double average_fill_price = 0.0;
    std::string rejection_reason;
    std::chrono::system_clock::time_point updated_at{};
    bool locally_owned = false;
    bool requires_reconciliation = false;
    std::string base_asset;
    std::string quote_asset;
    std::vector<std::string> client_order_aliases;
    std::int64_t last_event_time_ms = 0;
    std::map<std::int64_t, ExecutionCommission> commissions;
    std::map<std::int64_t, double> execution_trade_quantities;

    bool exchange_confirmed_fill() const noexcept {
        return state == State::Filled && exchange_confirmed_execution();
    }

    bool exchange_confirmed_execution() const noexcept {
        return exchange_order_id > 0 && std::isfinite(executed_quantity) && executed_quantity > 0.0 &&
            std::isfinite(cumulative_quote_quantity) && cumulative_quote_quantity > 0.0 &&
            std::isfinite(average_fill_price) && average_fill_price > 0.0;
    }
};

} // namespace sentum::order
