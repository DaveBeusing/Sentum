#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace sentum::order {

enum class Side { Buy, Sell };
enum class State { Pending, Acknowledged, PartiallyFilled, Filled, Cancelling, Cancelled, Rejected };

inline const char* to_string(State state) {
    switch (state) {
        case State::Pending: return "pending";
        case State::Acknowledged: return "acknowledged";
        case State::PartiallyFilled: return "partially_filled";
        case State::Filled: return "filled";
        case State::Cancelling: return "cancelling";
        case State::Cancelled: return "cancelled";
        case State::Rejected: return "rejected";
    }
    return "unknown";
}

struct Request {
    std::string symbol;
    Side side = Side::Buy;
    double quantity = 0.0;
    std::string client_order_id;
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

    bool exchange_confirmed_fill() const noexcept {
        return state == State::Filled && exchange_order_id > 0 && executed_quantity > 0.0;
    }
};

} // namespace sentum::order
