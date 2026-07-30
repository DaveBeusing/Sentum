#pragma once

#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include <sentum/trader/order/OrderTypes.hpp>

namespace sentum::order {

struct ConfirmedPosition {
    std::string symbol;
    double quantity = 0.0;
    double average_entry_price = 0.0;
    std::int64_t entry_exchange_order_id = 0;
    std::string entry_client_order_id;
};

// This ledger is deliberately downstream of OrderManager. It never consumes
// order intents or REST acknowledgements; only exchange-confirmed FILLED events.
class ConfirmedPositionLedger {
public:
    void on_order_update(const Snapshot& order) {
        if (!order.exchange_confirmed_fill()) return;
        std::lock_guard<std::mutex> lock(mutex_);
        if (order.side == Side::Buy) {
            auto& position = positions_[order.symbol];
            const double previous_quote = position.quantity * position.average_entry_price;
            const double added_quote = order.executed_quantity * order.average_fill_price;
            position.symbol = order.symbol;
            position.quantity += order.executed_quantity;
            position.average_entry_price = position.quantity > 0.0
                ? (previous_quote + added_quote) / position.quantity : 0.0;
            position.entry_exchange_order_id = order.exchange_order_id;
            position.entry_client_order_id = order.client_order_id;
            return;
        }

        auto it = positions_.find(order.symbol);
        if (it == positions_.end()) return; // reconciliation may reveal inventory not created by Sentum
        it->second.quantity -= order.executed_quantity;
        if (it->second.quantity <= 1e-12) positions_.erase(it);
    }

    std::optional<ConfirmedPosition> get(const std::string& symbol) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = positions_.find(symbol);
        if (it == positions_.end()) return std::nullopt;
        return it->second;
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, ConfirmedPosition> positions_;
};

} // namespace sentum::order
