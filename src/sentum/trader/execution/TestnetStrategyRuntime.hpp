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

#include <sentum/api/BinanceWebsocketClient.hpp>
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
        status_.set("mode", "testnet");
        status_.set("symbol", symbol_);
        status_.set("kill_switch_active", false);
        status_.set("reconciliation_complete", false);
        venue_->start([this](const order::Snapshot& update) { on_order_update(update); });
        status_.set("reconciliation_complete", venue_->ready());
        price_stream_ = std::make_unique<BinanceWebsocketClient>(symbol_);
        price_stream_->set_on_price([this](double price) { on_price(price); });
        price_stream_->start();
        status_.set("market_data_connected", true);
        status_.set("user_stream_connected", true);
    }

    void stop() noexcept {
        if (!running_.exchange(false)) return;
        if (price_stream_) price_stream_->stop();
        if (venue_) venue_->stop();
        status_.set("market_data_connected", false);
        status_.set("user_stream_connected", false);
        status_.set("kill_switch_active", true);
    }

    bool running() const noexcept { return running_.load(); }
    void kill() noexcept { if (venue_) venue_->kill(); status_.set("kill_switch_active", true); }

private:
    static std::string id(const std::string& symbol, const char* side) {
        const auto now = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        return "sentum-" + symbol + "-" + side + "-" + std::to_string(now);
    }

    void on_price(double price) {
        if (!running_.load() || !venue_->ready() || price <= 0.0) return;
        const auto now = std::chrono::system_clock::now();
        status_.set("last_market_event_age_ms", 0);
        status_.set("last_price", price);

        std::lock_guard<std::mutex> lock(mutex_);
        if (active_order_) return;

        if (confirmed_quantity_ <= 0.0) {
            const auto signal = strategy_->on_price(price, now);
            if (signal.action != TradeAction::BUY) return;
            const auto decision = risk_manager_.approve_entry(signal, price, now, last_exit_);
            status_.set("last_signal", signal.reason);
            status_.set("last_risk_decision", decision.reason);
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
        try { events_.save(update, "exchange"); } catch (...) { venue_->kill(); }
        status_.set("last_order_state", order::to_string(update.state));
        status_.set("orders_pending", update.state == order::State::Pending || update.state == order::State::Acknowledged ? 1 : 0);
        status_.set("orders_partially_filled", update.state == order::State::PartiallyFilled ? 1 : 0);
        status_.set("last_user_event_age_ms", 0);

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
            status_.set("confirmed_position_quantity", confirmed_quantity_);
            status_.set("confirmed_entry_price", entry_price_);
        } else {
            confirmed_quantity_ = 0.0;
            entry_price_ = 0.0;
            entry_time_ = {};
            last_exit_ = update.updated_at;
            strategy_->reset();
            status_.set("confirmed_position_quantity", 0.0);
            status_.set("last_exit_reason", exit_reason_);
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
