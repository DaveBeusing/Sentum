#pragma once

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>

#include <sentum/api/BinanceWebsocketClient.hpp>
#include <sentum/dashboard/DashboardState.hpp>
#include <sentum/observability/StatusReporter.hpp>
#include <sentum/trader/execution/IExecutionVenue.hpp>
#include <sentum/trader/order/OrderEventRepository.hpp>
#include <sentum/trader/risk/RiskManager.hpp>
#include <sentum/trader/strategy/IStrategy.hpp>
#include <sentum/trader/types/RiskConfig.hpp>

namespace sentum::execution {

class TestnetStrategyRuntime {
public:
    TestnetStrategyRuntime(std::string symbol, RiskConfig risk,
        std::unique_ptr<IStrategy> strategy, std::unique_ptr<IExecutionVenue> venue)
        : symbol_(std::move(symbol)), risk_(risk), strategy_(std::move(strategy)),
          venue_(std::move(venue)), risk_manager_(risk_), events_("log/klines.sqlite3") {}

    ~TestnetStrategyRuntime() { stop(); }

    void start() {
        if (running_.exchange(true)) return;
        set_status("mode", "testnet");
        set_status("symbol", symbol_);
        set_status("kill_switch_active", false);
        set_status("reconciliation_complete", false);
        venue_->start([this](const order::Snapshot& update) { on_order_update(update); });
        set_status("reconciliation_complete", venue_->ready());
        price_stream_ = std::make_unique<BinanceWebsocketClient>(symbol_);
        price_stream_->set_on_price([this](double price) { on_price(price); });
        price_stream_->start();
        set_status("market_data_connected", true);
        set_status("user_stream_connected", true);
        sentum::dashboard::DashboardState::global().set("health", "healthy");
    }

    void stop() noexcept {
        if (!running_.exchange(false)) return;
        if (price_stream_) price_stream_->stop();
        if (venue_) venue_->stop();
        set_status("market_data_connected", false);
        set_status("user_stream_connected", false);
        set_status("kill_switch_active", true);
        sentum::dashboard::DashboardState::global().set("health", "stopped");
    }

    bool running() const noexcept { return running_.load(); }
    void kill() noexcept {
        if (venue_) venue_->kill();
        set_status("kill_switch_active", true);
        sentum::dashboard::DashboardState::global().set("health", "blocked");
    }

private:
    template <typename T>
    void set_status(const std::string& key, T&& value) {
        nlohmann::json json_value = std::forward<T>(value);
        status_.set(key, json_value);
        sentum::dashboard::DashboardState::global().set(key, std::move(json_value));
    }

    static std::string id(const std::string& symbol, const char* side) {
        const auto now = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        return "sentum-" + symbol + "-" + side + "-" + std::to_string(now);
    }

    void on_price(double price) {
        if (!running_.load() || !venue_->ready() || price <= 0.0) return;
        const auto now = std::chrono::system_clock::now();
        set_status("last_market_event_age_ms", 0);
        set_status("last_price", price);

        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_order_.empty()) return;

        if (confirmed_quantity_ <= 0.0) {
            const auto signal = strategy_->on_price(price, now);
            if (signal.action != TradeAction::BUY) return;
            const auto decision = risk_manager_.approve_entry(signal, price, now, last_exit_);
            set_status("last_signal", signal.reason);
            set_status("last_risk_decision", decision.reason);
            if (!decision.approved) return;
            order::Request request{symbol_, order::Side::Buy, decision.quantity, id(symbol_, "buy")};
            active_order_ = request.client_order_id;
            venue_->submit(request);
            return;
        }

        if (entry_price_ <= 0.0) return;
        const bool stop = price <= entry_price_ * (1.0 - risk_.stop_loss_percent);
        const bool target = price >= entry_price_ * (1.0 + risk_.take_profit_percent);
        const bool timeout = entry_time_ != std::chrono::system_clock::time_point{} &&
            now - entry_time_ >= std::chrono::seconds(risk_.max_holding_seconds);
        if (!stop && !target && !timeout) return;
        exit_reason_ = stop ? "stop_loss" : (target ? "take_profit" : "maximum_holding_time");
        order::Request request{symbol_, order::Side::Sell, confirmed_quantity_, id(symbol_, "sell")};
        active_order_ = request.client_order_id;
        venue_->submit(request);
    }

    void on_order_update(const order::Snapshot& update) {
        try { events_.save(update, "exchange"); }
        catch (...) { venue_->kill(); set_status("kill_switch_active", true); }
        set_status("last_order_state", order::to_string(update.state));
        set_status("orders_pending", update.state == order::State::Pending || update.state == order::State::Acknowledged ? 1 : 0);
        set_status("orders_partially_filled", update.state == order::State::PartiallyFilled ? 1 : 0);
        set_status("last_user_event_age_ms", 0);

        std::lock_guard<std::mutex> lock(mutex_);
        if (update.state == order::State::Rejected || update.state == order::State::Cancelled) {
            active_order_.clear();
            return;
        }
        if (!update.exchange_confirmed_fill()) return;
        if (update.side == order::Side::Buy) {
            confirmed_quantity_ = update.executed_quantity;
            entry_price_ = update.average_fill_price;
            entry_time_ = update.updated_at;
            set_status("confirmed_position_quantity", confirmed_quantity_);
            set_status("confirmed_entry_price", entry_price_);
            sentum::dashboard::DashboardState::global().set("active_position",
                nlohmann::json{{"symbol",symbol_},{"quantity",confirmed_quantity_},{"entry_price",entry_price_}});
        } else {
            confirmed_quantity_ = 0.0;
            entry_price_ = 0.0;
            entry_time_ = {};
            last_exit_ = update.updated_at;
            strategy_->reset();
            set_status("confirmed_position_quantity", 0.0);
            set_status("last_exit_reason", exit_reason_);
            sentum::dashboard::DashboardState::global().set("active_position", nullptr);
        }
        active_order_.clear();
    }

    std::string symbol_;
    RiskConfig risk_;
    std::unique_ptr<IStrategy> strategy_;
    std::unique_ptr<IExecutionVenue> venue_;
    RiskManager risk_manager_;
    order::OrderEventRepository events_;
    observability::StatusReporter status_;
    std::unique_ptr<BinanceWebsocketClient> price_stream_;
    std::atomic<bool> running_{false};
    std::mutex mutex_;
    std::string active_order_;
    std::string exit_reason_;
    double confirmed_quantity_ = 0.0;
    double entry_price_ = 0.0;
    std::chrono::system_clock::time_point entry_time_{};
    std::chrono::system_clock::time_point last_exit_{};
};

} // namespace sentum::execution
