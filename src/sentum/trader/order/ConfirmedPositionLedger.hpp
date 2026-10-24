#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <sentum/trader/order/OrderTypes.hpp>

namespace sentum::order {

struct ConfirmedPosition {
    std::string symbol;
    double quantity = 0.0;
    double average_entry_price = 0.0;
    std::int64_t entry_exchange_order_id = 0;
    std::string entry_client_order_id;
};

struct AppliedExecutionDelta {
    bool applied = false;
    double executed_quantity = 0.0;
    double quote_quantity = 0.0;
    double base_quantity_change = 0.0;
    double quote_balance_change = 0.0;
    std::map<std::string, double> commissions;
};

// The ledger consumes exchange-confirmed cumulative executions of owned orders.
// State labels do not decide exposure: a cancelled order can retain a real fill.
class ConfirmedPositionLedger {
public:
    // Validate a complete chronological replay before replacing any live state.
    void restore(const std::vector<Snapshot>& replay) {
        ConfirmedPositionLedger recovered;
        for (const auto& snapshot : replay) recovered.on_order_update(snapshot);
        std::lock_guard<std::mutex> lock(mutex_);
        positions_.swap(recovered.positions_);
        assets_.swap(recovered.assets_);
        applied_.swap(recovered.applied_);
        client_identities_.swap(recovered.client_identities_);
        execution_order_.swap(recovered.execution_order_);
        commission_totals_.swap(recovered.commission_totals_);
    }

    AppliedExecutionDelta on_order_update(const Snapshot& order) {
        if (!order.locally_owned) return {};
        validate_numbers(order);
        if (order.executed_quantity == 0.0 && order.commissions.empty()) return {};
        if (order.symbol.empty() || order.client_order_id.empty() || order.exchange_order_id <= 0)
            throw std::runtime_error("Confirmed execution has no stable order identity");

        std::lock_guard<std::mutex> lock(mutex_);
        const auto key = identity(order);
        const auto client = client_identities_.find(order.client_order_id);
        if (client != client_identities_.end() && client->second != key)
            throw std::runtime_error("Client order identity changed its exchange order");
        const auto existing = applied_.find(key);
        const bool new_order = existing == applied_.end();
        Watermark next;
        if (existing != applied_.end()) {
            next = existing->second;
            if (next.client_order_id != order.client_order_id || next.side != order.side)
                throw std::runtime_error("Confirmed execution changed order ownership");
            merge_asset(next.base_asset, order.base_asset);
            merge_asset(next.quote_asset, order.quote_asset);
        } else {
            next.client_order_id = order.client_order_id;
            next.exchange_order_id = order.exchange_order_id;
            next.side = order.side;
            next.base_asset = order.base_asset;
            next.quote_asset = order.quote_asset;
        }

        auto symbol_assets = assets_.find(order.symbol) == assets_.end()
            ? std::pair<std::string, std::string>{} : assets_.at(order.symbol);
        merge_asset(symbol_assets.first, next.base_asset);
        merge_asset(symbol_assets.second, next.quote_asset);
        if (!symbol_assets.first.empty() && symbol_assets.first == symbol_assets.second)
            throw std::runtime_error("Confirmed execution has identical base and quote assets");

        AppliedExecutionDelta delta;
        const double previous_quantity = next.quantity;
        const double previous_quote = next.quote;
        // Older reports may contribute a previously unseen commission trade,
        // but can never reduce an execution or quote watermark.
        if (order.executed_quantity >= next.quantity && order.cumulative_quote_quantity >= next.quote) {
            delta.executed_quantity = order.executed_quantity - next.quantity;
            delta.quote_quantity = order.cumulative_quote_quantity - next.quote;
            if ((delta.executed_quantity == 0.0) != (delta.quote_quantity == 0.0))
                throw std::runtime_error("Execution quantity and quote did not advance together");
            next.quantity = order.executed_quantity;
            next.quote = order.cumulative_quote_quantity;
        } else if (order.executed_quantity > next.quantity || order.cumulative_quote_quantity > next.quote) {
            throw std::runtime_error("Contradictory cumulative execution quantities");
        }

        for (const auto& [trade_id, fee] : order.commissions) {
            if (trade_id < 0 || (fee.asset.empty() && fee.amount != 0.0) || !finite_nonnegative(fee.amount))
                throw std::runtime_error("Invalid execution commission");
            const auto seen = next.commissions.find(trade_id);
            if (seen != next.commissions.end()) {
                if (seen->second.asset != fee.asset || seen->second.amount != fee.amount)
                    throw std::runtime_error("Execution commission changed for an existing trade");
                continue;
            }
            next.commissions.emplace(trade_id, fee);
            if (fee.amount != 0.0) {
                delta.commissions[fee.asset] += fee.amount;
                if (!std::isfinite(delta.commissions[fee.asset]))
                    throw std::runtime_error("Execution commissions overflowed");
            }
        }
        const bool has_positive_fee = std::any_of(next.commissions.begin(), next.commissions.end(),
            [](const auto& fee) { return fee.second.amount > 0.0; });
        if (has_positive_fee &&
            (next.base_asset.empty() || next.quote_asset.empty() || next.base_asset == next.quote_asset))
            throw std::runtime_error("Execution commission requires explicit base and quote assets");

        const auto has_new_fees = existing == applied_.end()
            ? !next.commissions.empty() : next.commissions.size() != existing->second.commissions.size();
        if (next.quantity == previous_quantity && next.quote == previous_quote && !has_new_fees) return {};

        const double base_fee = fee_amount(delta.commissions, next.base_asset);
        const double quote_fee = fee_amount(delta.commissions, next.quote_asset);
        delta.base_quantity_change = (next.side == Side::Buy ? delta.executed_quantity : -delta.executed_quantity) - base_fee;
        delta.quote_balance_change = (next.side == Side::Buy ? -delta.quote_quantity : delta.quote_quantity) - quote_fee;
        if (!std::isfinite(delta.base_quantity_change) || !std::isfinite(delta.quote_balance_change))
            throw std::runtime_error("Execution balance delta overflowed");

        auto position = position_or_empty(order.symbol);
        const auto sequence_it = execution_order_.find(order.symbol);
        const bool historical = existing != applied_.end() && sequence_it != execution_order_.end() &&
            !sequence_it->second.empty() && sequence_it->second.back() != key;
        if (historical) {
            // A sequential strategy cannot acquire new execution on an earlier
            // settled order after submitting another order. Preserve its last
            // known state and require reconciliation if that invariant breaks.
            if (delta.executed_quantity != 0.0 || delta.quote_quantity != 0.0)
                throw std::runtime_error("Late execution crosses an order boundary; reconciliation required");
            // A delayed trade report can add an already charged commission.
            // Replay the symbol's canonical order totals so that fee changes
            // the original acquisition cost, not a later independent position.
            position = ConfirmedPosition{};
            position.symbol = order.symbol;
            for (const auto& prior_key : sequence_it->second) {
                const auto& prior = prior_key == key ? next : applied_.at(prior_key);
                apply_to_position(position, prior, prior.quantity, prior.quote, fees(prior));
            }
        } else {
            apply_to_position(position, next, delta.executed_quantity, delta.quote_quantity, delta.commissions);
        }

        const auto existing_totals = commission_totals_.find(order.symbol);
        auto totals = existing_totals == commission_totals_.end()
            ? std::map<std::string, double>{} : existing_totals->second;
        for (const auto& [asset, amount] : delta.commissions) {
            totals[asset] += amount;
            if (!std::isfinite(totals[asset])) throw std::runtime_error("Commission total overflowed");
        }
        // Commit only after identity, quantities, metadata and inventory have
        // all been checked; failed validation cannot consume a watermark.
        applied_.insert_or_assign(key, std::move(next));
        client_identities_.insert_or_assign(order.client_order_id, key);
        if (new_order) execution_order_[order.symbol].push_back(key);
        commission_totals_.insert_or_assign(order.symbol, std::move(totals));
        assets_.insert_or_assign(order.symbol, std::move(symbol_assets));
        if (position.quantity == 0.0) positions_.erase(order.symbol);
        else positions_.insert_or_assign(order.symbol, std::move(position));
        delta.applied = true;
        return delta;
    }

    std::optional<ConfirmedPosition> get(const std::string& symbol) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = positions_.find(symbol);
        return it == positions_.end() ? std::nullopt : std::optional<ConfirmedPosition>(it->second);
    }

    std::map<std::string, double> commission_totals(const std::string& symbol) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = commission_totals_.find(symbol);
        return it == commission_totals_.end() ? std::map<std::string, double>{} : it->second;
    }

private:
    struct Watermark {
        std::string client_order_id;
        std::int64_t exchange_order_id = 0;
        Side side = Side::Buy;
        std::string base_asset, quote_asset;
        double quantity = 0.0, quote = 0.0;
        std::map<std::int64_t, ExecutionCommission> commissions;
    };

    static bool finite_nonnegative(double value) { return std::isfinite(value) && value >= 0.0; }
    static void validate_numbers(const Snapshot& order) {
        if (!finite_nonnegative(order.executed_quantity) || !finite_nonnegative(order.cumulative_quote_quantity) ||
            !finite_nonnegative(order.requested_quantity) || !finite_nonnegative(order.average_fill_price))
            throw std::runtime_error("Invalid cumulative execution quantity");
        if ((order.executed_quantity == 0.0) != (order.cumulative_quote_quantity == 0.0))
            throw std::runtime_error("Incomplete cumulative execution quantity");
        if (order.executed_quantity > 0.0 && !order.exchange_confirmed_execution())
            throw std::runtime_error("Execution lacks exchange confirmation");
    }

    static std::string identity(const Snapshot& order) {
        return order.symbol + '\0' + std::to_string(order.exchange_order_id);
    }
    static void merge_asset(std::string& known, const std::string& observed) {
        if (observed.empty()) return;
        if (!known.empty() && known != observed) throw std::runtime_error("Execution asset metadata changed");
        known = observed;
    }
    static double fee_amount(const std::map<std::string, double>& fees, const std::string& asset) {
        const auto it = fees.find(asset);
        return it == fees.end() ? 0.0 : it->second;
    }
    static std::map<std::string, double> fees(const Watermark& watermark) {
        std::map<std::string, double> totals;
        for (const auto& [id, fee] : watermark.commissions) {
            if (fee.amount == 0.0) continue;
            totals[fee.asset] += fee.amount;
            if (!std::isfinite(totals[fee.asset])) throw std::runtime_error("Commission total overflowed");
        }
        return totals;
    }
    ConfirmedPosition position_or_empty(const std::string& symbol) const {
        const auto it = positions_.find(symbol);
        if (it != positions_.end()) return it->second;
        ConfirmedPosition position;
        position.symbol = symbol;
        return position;
    }
    static void apply_to_position(ConfirmedPosition& position, const Watermark& order,
                                  double quantity, double quote, const std::map<std::string, double>& fees) {
        const double base_fee = fee_amount(fees, order.base_asset);
        if (order.side == Side::Buy) {
            const double net_quantity = position.quantity + quantity - base_fee;
            const double cost = position.quantity * position.average_entry_price + quote + fee_amount(fees, order.quote_asset);
            if (!std::isfinite(net_quantity) || !std::isfinite(cost) || net_quantity <= 0.0 || cost <= 0.0)
                throw std::runtime_error("Invalid net confirmed acquisition");
            position.quantity = net_quantity;
            position.average_entry_price = cost / net_quantity;
            if (!std::isfinite(position.average_entry_price)) throw std::runtime_error("Confirmed entry price overflowed");
            position.entry_exchange_order_id = order.exchange_order_id;
            position.entry_client_order_id = order.client_order_id;
        } else {
            const double reduction = quantity + base_fee;
            const double remaining = position.quantity - reduction;
            const double tolerance = 32.0 * std::numeric_limits<double>::epsilon() * std::max(position.quantity, reduction);
            if (!std::isfinite(reduction) || !std::isfinite(remaining) || remaining < -tolerance)
                throw std::runtime_error("Confirmed sale exceeds owned inventory; reconciliation required");
            position.quantity = remaining <= tolerance ? 0.0 : remaining;
            if (position.quantity == 0.0) position.average_entry_price = 0.0;
        }
    }

    mutable std::mutex mutex_;
    std::unordered_map<std::string, ConfirmedPosition> positions_;
    std::unordered_map<std::string, std::pair<std::string, std::string>> assets_;
    std::unordered_map<std::string, Watermark> applied_;
    std::unordered_map<std::string, std::string> client_identities_;
    std::unordered_map<std::string, std::vector<std::string>> execution_order_;
    std::unordered_map<std::string, std::map<std::string, double>> commission_totals_;
};

} // namespace sentum::order
