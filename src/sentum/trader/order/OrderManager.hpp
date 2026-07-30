#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include <sentum/api/BinanceSpotExecutionClient.hpp>
#include <sentum/trader/order/OrderTypes.hpp>

namespace sentum::order {

class OrderManager {
public:
    using UpdateHandler = std::function<void(const Snapshot&)>;

    explicit OrderManager(BinanceSpotExecutionClient& exchange) : exchange_(exchange) {}

    void set_update_handler(UpdateHandler handler) {
        std::lock_guard<std::mutex> lock(mutex_);
        handler_ = std::move(handler);
    }

    void reconcile_startup() {
        const auto open = exchange_.open_orders();
        if (!open.is_array()) throw std::runtime_error("Invalid open-orders reconciliation response");
        std::lock_guard<std::mutex> lock(mutex_);
        orders_.clear();
        for (const auto& value : open) {
            Snapshot snapshot = from_rest(value);
            orders_[snapshot.client_order_id] = snapshot;
        }
        reconciled_.store(true);
    }

    Snapshot submit_market(const Request& request) {
        if (!reconciled_.load()) throw std::logic_error("Orders cannot be submitted before startup reconciliation");
        if (kill_switch_.load()) throw std::logic_error("Kill switch is active");
        if (request.quantity <= 0.0 || request.symbol.empty() || request.client_order_id.empty()) {
            throw std::invalid_argument("Invalid order request");
        }
        Snapshot pending;
        pending.symbol = request.symbol;
        pending.client_order_id = request.client_order_id;
        pending.side = request.side;
        pending.requested_quantity = request.quantity;
        pending.state = State::Pending;
        pending.updated_at = std::chrono::system_clock::now();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (orders_.count(request.client_order_id)) throw std::logic_error("Duplicate client order id");
            orders_[request.client_order_id] = pending;
        }
        publish(pending);

        try {
            const auto ack = exchange_.place_market_order(request);
            std::lock_guard<std::mutex> lock(mutex_);
            auto& current = orders_.at(request.client_order_id);
            current.exchange_order_id = ack.value("orderId", std::int64_t{0});
            current.state = State::Acknowledged;
            current.updated_at = std::chrono::system_clock::now();
            Snapshot copy = current;
            publish_unlocked(copy);
            return copy;
        } catch (const std::exception& error) {
            std::lock_guard<std::mutex> lock(mutex_);
            auto& current = orders_.at(request.client_order_id);
            current.state = State::Rejected;
            current.rejection_reason = error.what();
            current.updated_at = std::chrono::system_clock::now();
            Snapshot copy = current;
            publish_unlocked(copy);
            return copy;
        }
    }

    Snapshot cancel(const std::string& client_order_id) {
        Snapshot cancelling;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = orders_.find(client_order_id);
            if (it == orders_.end()) throw std::out_of_range("Unknown client order id");
            if (it->second.state == State::Filled || it->second.state == State::Cancelled || it->second.state == State::Rejected) return it->second;
            it->second.state = State::Cancelling;
            it->second.updated_at = std::chrono::system_clock::now();
            cancelling = it->second;
        }
        publish(cancelling);
        exchange_.cancel_order(cancelling.symbol, cancelling.client_order_id);
        return cancelling; // final cancellation is accepted only from executionReport or reconciliation
    }

    void activate_kill_switch() {
        kill_switch_.store(true);
        std::vector<Snapshot> cancellable;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& [id, order] : orders_) {
                if (order.state == State::Pending || order.state == State::Acknowledged || order.state == State::PartiallyFilled) cancellable.push_back(order);
            }
        }
        for (const auto& order : cancellable) {
            try { cancel(order.client_order_id); } catch (...) { /* remain blocked; reconciliation resolves status */ }
        }
    }

    bool kill_switch_active() const noexcept { return kill_switch_.load(); }
    bool reconciled() const noexcept { return reconciled_.load(); }

    std::optional<Snapshot> get(const std::string& client_order_id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = orders_.find(client_order_id);
        if (it == orders_.end()) return std::nullopt;
        return it->second;
    }

    void on_user_data_event(const nlohmann::json& event) {
        if (event.value("e", std::string{}) != "executionReport") return;
        const std::string client_id = event.value("c", std::string{});
        if (client_id.empty()) return;
        Snapshot updated;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto& current = orders_[client_id];
            current.symbol = event.value("s", current.symbol);
            current.client_order_id = client_id;
            current.exchange_order_id = event.value("i", current.exchange_order_id);
            current.side = event.value("S", std::string{"BUY"}) == "SELL" ? Side::Sell : Side::Buy;
            current.requested_quantity = parse_number(event, "q", current.requested_quantity);
            current.executed_quantity = parse_number(event, "z", current.executed_quantity);
            current.cumulative_quote_quantity = parse_number(event, "Z", current.cumulative_quote_quantity);
            current.average_fill_price = current.executed_quantity > 0.0
                ? current.cumulative_quote_quantity / current.executed_quantity : 0.0;
            current.state = map_status(event.value("X", std::string{}));
            current.rejection_reason = event.value("r", std::string{});
            current.updated_at = std::chrono::system_clock::time_point(
                std::chrono::milliseconds(event.value("E", std::int64_t{0})));
            updated = current;
        }
        publish(updated);
    }

private:
    static double parse_number(const nlohmann::json& value, const char* key, double fallback) {
        if (!value.contains(key)) return fallback;
        try {
            return value.at(key).is_string() ? std::stod(value.at(key).get<std::string>()) : value.at(key).get<double>();
        } catch (...) { return fallback; }
    }

    static State map_status(const std::string& status) {
        if (status == "NEW") return State::Acknowledged;
        if (status == "PARTIALLY_FILLED") return State::PartiallyFilled;
        if (status == "FILLED") return State::Filled;
        if (status == "PENDING_CANCEL") return State::Cancelling;
        if (status == "CANCELED" || status == "EXPIRED") return State::Cancelled;
        if (status == "REJECTED") return State::Rejected;
        return State::Pending;
    }

    static Snapshot from_rest(const nlohmann::json& value) {
        Snapshot result;
        result.symbol = value.value("symbol", std::string{});
        result.client_order_id = value.value("clientOrderId", std::string{});
        result.exchange_order_id = value.value("orderId", std::int64_t{0});
        result.side = value.value("side", std::string{"BUY"}) == "SELL" ? Side::Sell : Side::Buy;
        result.requested_quantity = parse_number(value, "origQty", 0.0);
        result.executed_quantity = parse_number(value, "executedQty", 0.0);
        result.cumulative_quote_quantity = parse_number(value, "cummulativeQuoteQty", 0.0);
        result.average_fill_price = result.executed_quantity > 0.0 ? result.cumulative_quote_quantity / result.executed_quantity : 0.0;
        result.state = map_status(value.value("status", std::string{}));
        result.updated_at = std::chrono::system_clock::now();
        return result;
    }

    void publish(const Snapshot& value) {
        UpdateHandler copy;
        { std::lock_guard<std::mutex> lock(mutex_); copy = handler_; }
        if (copy) copy(value);
    }

    void publish_unlocked(const Snapshot& value) {
        auto copy = handler_;
        if (copy) copy(value);
    }

    BinanceSpotExecutionClient& exchange_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Snapshot> orders_;
    UpdateHandler handler_;
    std::atomic<bool> reconciled_{false};
    std::atomic<bool> kill_switch_{false};
};

} // namespace sentum::order
