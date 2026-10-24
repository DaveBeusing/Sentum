#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <type_traits>

namespace sentum::risk {

// Both execution environments advance the same stop/target/holding policy.
// Prices and entry time are supplied by their confirmed execution state.
inline const char* evaluate_position_exit(double price,
    std::chrono::system_clock::time_point now,
    std::chrono::system_clock::time_point entry_time,
    double& highest_price, double& lowest_price, double& stop_loss_price,
    double take_profit_price, bool trailing_enabled, double trailing_percent,
    std::chrono::seconds maximum_holding) {
    if (!std::isfinite(price) || price <= 0.0 || !std::isfinite(highest_price) || highest_price <= 0.0 ||
        !std::isfinite(lowest_price) || lowest_price <= 0.0 ||
        !std::isfinite(stop_loss_price) || stop_loss_price <= 0.0 ||
        !std::isfinite(take_profit_price) || take_profit_price <= 0.0 ||
        (trailing_enabled && (!std::isfinite(trailing_percent) || trailing_percent <= 0.0 || trailing_percent >= 1.0)) ||
        maximum_holding <= std::chrono::seconds::zero())
        throw std::invalid_argument("Invalid confirmed position exit policy");
    if (price > highest_price) {
        highest_price = price;
        if (trailing_enabled)
            stop_loss_price = std::max(stop_loss_price, price * (1.0 - trailing_percent));
    }
    lowest_price = std::min(lowest_price, price);
    if (price <= stop_loss_price) return "stop_loss";
    if (price >= take_profit_price) return "take_profit";
    if (entry_time != std::chrono::system_clock::time_point{} && now >= entry_time) {
        using Ticks = std::make_unsigned_t<std::chrono::system_clock::duration::rep>;
        const auto elapsed = static_cast<Ticks>(now.time_since_epoch().count()) -
            static_cast<Ticks>(entry_time.time_since_epoch().count());
        const auto bound = std::chrono::duration_cast<std::chrono::system_clock::duration>(maximum_holding).count();
        if (elapsed >= static_cast<Ticks>(bound)) return "maximum_holding_time";
    }
    return nullptr;
}

} // namespace sentum::risk
