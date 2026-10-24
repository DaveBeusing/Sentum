#pragma once

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <unordered_map>

#include <sentum/api/BinanceWebsocketClient.hpp>
#include <sentum/dashboard/DashboardState.hpp>
#include <sentum/observability/StatusReporter.hpp>
#include <sentum/trader/execution/IExecutionVenue.hpp>
#include <sentum/trader/order/OrderEventRepository.hpp>
#include <sentum/trader/order/ConfirmedPositionLedger.hpp>
#include <sentum/trader/risk/RiskManager.hpp>
#include <sentum/trader/risk/PositionExitPolicy.hpp>
#include <sentum/trader/risk/PositionPolicyCheckpoint.hpp>
#include <sentum/trader/strategy/IStrategy.hpp>
#include <sentum/trader/types/RiskConfig.hpp>

namespace sentum::execution {

class TestnetStrategyRuntime {
public:
    TestnetStrategyRuntime(std::string symbol, RiskConfig risk,
        std::unique_ptr<IStrategy> strategy, std::unique_ptr<IExecutionVenue> venue,
        std::shared_ptr<IClock> clock = std::make_shared<SystemClock>())
        : symbol_(std::move(symbol)), risk_(risk), strategy_(std::move(strategy)),
          venue_(std::move(venue)), clock_(std::move(clock)), risk_manager_(risk_, clock_), events_("log/klines.sqlite3") {
        if (!strategy_ || !venue_ || !clock_) throw std::invalid_argument("Testnet runtime requires a strategy, venue and clock");
        if (venue_->market_environment() != api::BinanceEnvironment::SpotTestnet)
            throw std::invalid_argument("Testnet execution requires Testnet market provenance");
    }

    ~TestnetStrategyRuntime() { stop(); }

    void start() {
        if (running_.exchange(true)) return;
        try {
            set_status("mode", "testnet");
            set_status("symbol", symbol_);
            set_status("kill_switch_active", venue_->killed());
            set_status("reconciliation_complete", false);
            venue_->configure_market(symbol_, risk_);
            venue_->set_recovery_handler([this](const ExecutionRecoveryState& recovered) { on_recovery(recovered); });
            venue_->set_health_handler([this](bool healthy) {
                set_status("user_stream_connected", healthy);
                set_status("reconciliation_complete", venue_->reconciliation_complete());
                set_status("kill_switch_active", venue_->killed());
                sentum::dashboard::DashboardState::global().set("health", venue_->ready() ? "healthy" : "blocked");
            });
            venue_->start([this](const order::Snapshot& update) { on_order_update(update); });
            set_status("reconciliation_complete", venue_->reconciliation_complete());
            set_status("kill_switch_active", venue_->killed());
            price_stream_ = std::make_unique<BinanceWebsocketClient>(symbol_, std::chrono::milliseconds(risk_.max_data_age_ms),
                venue_->market_environment());
            set_status("market_data_environment", api::environment_name(price_stream_->environment()));
            set_status("market_data_endpoint", price_stream_->endpoint());
            price_stream_->set_on_health([this](const sentum::api::MarketStreamHealth& health) {
                set_status("market_data_connected", health.connected);
                set_status("market_data_fresh", health.fresh);
                set_status("last_market_event_age_ms", health.last_event_age_ms);
                set_status("price_market_stream", health.json());
            });
            price_stream_->set_on_price([this](double price) { on_price(price); });
            price_stream_->start();
            set_status("market_data_connected", price_stream_->stream_health().connected);
            set_status("market_data_fresh", price_stream_->stream_health().fresh);
            set_status("user_stream_connected", venue_->user_stream_healthy());
            sentum::dashboard::DashboardState::global().set("health", venue_->ready() && !venue_->killed() ? "healthy" : "blocked");
        } catch (...) { stop(); throw; }
    }

    void stop() noexcept {
        if (!running_.exchange(false)) return;
        if (price_stream_) price_stream_->stop();
        if (venue_) venue_->stop();
        set_status("market_data_connected", false);
        set_status("market_data_fresh", false);
        set_status("user_stream_connected", false);
        set_status("kill_switch_active", true);
        sentum::dashboard::DashboardState::global().set("health", "stopped");
    }

    bool running() const noexcept { return running_.load(); }
    bool resume_after_reconcile(const std::string& confirmation) {
        if (!running_.load() || !venue_->resume_after_reconcile(confirmation)) return false;
        kill_requested_.store(false);
        return true;
    }
    void kill() noexcept {
        // Cancelling an order may synchronously report another update. Latch
        // first so a failed evidence write cannot recursively request a kill.
        if (!kill_requested_.exchange(true) && venue_) venue_->kill();
        try {
            set_status("kill_switch_active", true);
            sentum::dashboard::DashboardState::global().set("health", "blocked");
        } catch (...) {}
    }

private:
    friend struct TestnetStrategyRuntimeTestAccess;

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
        if (!running_.load() || kill_requested_.load() || !venue_->ready() || !std::isfinite(price) || price <= 0.0) return;
        const auto now = clock_->now();
        set_status("last_market_event_age_ms", 0);
        set_status("last_price", price);

        std::optional<order::Request> request;
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_.load() || kill_requested_.load() || reconciliation_pending_) return;
            const bool order_reserved = submission_in_progress_ || !active_order_.empty();

            if (confirmed_quantity_ <= 0.0) {
                if (order_reserved) return;
                const auto signal = strategy_->on_price(price, now);
                if (signal.action != TradeAction::BUY) return;
                const auto decision = risk_manager_.approve_entry(signal, price, now, last_exit_);
                set_status("last_signal", signal.reason);
                set_status("last_risk_decision", decision.reason);
                if (!decision.approved) return;
                request = order::Request{symbol_, order::Side::Buy, decision.quantity, id(symbol_, "buy")};
            } else {
                if (entry_price_ <= 0.0) return;
                double highest = highest_price_, lowest = lowest_price_, stop = stop_loss_price_;
                const auto reason = sentum::risk::evaluate_position_exit(price, now, entry_time_,
                    highest, lowest, stop, take_profit_price_,
                    risk_.trailing_sl_enabled, risk_.trailing_sl_percent, std::chrono::seconds(risk_.max_holding_seconds));
                if (highest != highest_price_ || lowest != lowest_price_ || stop != stop_loss_price_)
                    persist_policy_locked(current_policy_locked(highest, lowest, stop, take_profit_price_));
                // Partial inventory remains protected while its current order
                // resolves. A following order still requires that reservation
                // to be released by a terminal event or reconciliation.
                if (!reason || order_reserved) return;
                exit_reason_ = reason;
                request = order::Request{symbol_, order::Side::Sell, confirmed_quantity_, id(symbol_, "sell")};
            }
            active_order_ = request->client_order_id;
            owned_orders_.emplace(active_order_, OwnedOrder{request->side});
            submission_in_progress_ = true;
        } catch (const std::exception& error) {
            kill();
            try { set_status("last_execution_error", error.what()); } catch (...) {}
            return;
        } catch (...) {
            kill();
            try { set_status("last_execution_error", "Position policy persistence failed"); } catch (...) {}
            return;
        }

        // Ownership is reserved, but neither this mutex nor the venue's order
        // mutex may be held while synchronous callbacks are delivered. A
        // terminal callback can release active_order_ before submit returns;
        // submission_in_progress_ still prevents a competing price tick.
        try {
            (void)venue_->submit(*request);
        } catch (const std::exception& error) {
            kill();
            try { set_status("last_submission_error", error.what()); } catch (...) {}
        } catch (...) {
            kill();
            try { set_status("last_submission_error", "Unknown order submission failure"); } catch (...) {}
        }
        std::lock_guard<std::mutex> lock(mutex_);
        submission_in_progress_ = false;
        // The return value is not a second order event. In particular a late
        // ACK must not overwrite a terminal callback or recreate ownership.
    }

    void on_order_update(const order::Snapshot& update) {
        bool identity_conflict = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto owner = owned_orders_.find(update.client_order_id);
            if (!update.locally_owned || update.symbol != symbol_ || owner == owned_orders_.end()) return;
            identity_conflict = owner->second.side != update.side ||
                (owner->second.exchange_order_id > 0 && update.exchange_order_id > 0 &&
                 owner->second.exchange_order_id != update.exchange_order_id);
        }
        if (identity_conflict) {
            kill();
            try { set_status("last_execution_error", "Order identity changed; reconciliation required"); } catch (...) {}
            return;
        }

        // Evidence failures block submission immediately, outside the state
        // mutex: killing the venue can synchronously deliver cancellation.
        try { events_.save(update, "exchange"); }
        catch (...) { kill(); }

        bool show_update = false;
        bool reconcile_required = update.state == order::State::Unknown || update.requires_reconciliation;
        bool position_changed = false;
        double quantity = 0.0, entry_price = 0.0;
        bool exited = false;
        std::string exit_reason;
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            auto& owner = owned_orders_.at(update.client_order_id);
            if (owner.exchange_order_id > 0 && update.exchange_order_id > 0 &&
                owner.exchange_order_id != update.exchange_order_id)
                throw std::runtime_error("Order identity changed during execution update");
            const bool current_order = update.client_order_id == active_order_;
            const auto delta = position_ledger_.on_order_update(update);
            if (owner.exchange_order_id == 0 && update.exchange_order_id > 0)
                owner.exchange_order_id = update.exchange_order_id;
            if (delta.applied && (delta.executed_quantity != 0.0 || delta.quote_quantity != 0.0 || !delta.commissions.empty())) {
                const double previous_quantity = confirmed_quantity_;
                const auto position = position_ledger_.get(symbol_);
                confirmed_quantity_ = position ? position->quantity : 0.0;
                entry_price_ = position ? position->average_entry_price : 0.0;
                if (previous_quantity == 0.0 && confirmed_quantity_ > 0.0) {
                    entry_time_ = update.updated_at;
                    initialize_position_policy();
                } else if (confirmed_quantity_ > 0.0) {
                    persist_policy_locked(current_policy_locked(
                        std::max(highest_price_, entry_price_), std::min(lowest_price_, entry_price_),
                        std::max(stop_loss_price_, entry_price_ * (1.0 - risk_.stop_loss_percent)),
                        entry_price_ * (1.0 + risk_.take_profit_percent)));
                }
                exited = previous_quantity > 0.0 && confirmed_quantity_ == 0.0;
                if (exited) {
                    entry_time_ = {};
                    highest_price_ = lowest_price_ = stop_loss_price_ = take_profit_price_ = 0.0;
                    policy_checkpoint_.reset();
                    last_exit_ = update.updated_at;
                    strategy_->reset();
                }
                position_changed = true;
                // A newly discovered execution/fee on an already released
                // reservation is retained, but cannot silently change the
                // assumptions of a later in-flight strategy order.
                if (!current_order && !active_order_.empty()) reconcile_required = true;
            }
            const bool terminal = update.state == order::State::Filled || update.state == order::State::Cancelled ||
                update.state == order::State::Rejected;
            if (current_order && terminal) active_order_.clear();
            if (reconcile_required) reconciliation_pending_ = true;
            show_update = current_order || position_changed;
            quantity = confirmed_quantity_;
            entry_price = entry_price_;
            exit_reason = exit_reason_;
        } catch (const std::exception& error) {
            kill();
            try { set_status("last_execution_error", error.what()); } catch (...) {}
            return;
        }

        if (show_update) {
            set_status("last_order_state", order::to_string(update.state));
            set_status("orders_pending", update.state == order::State::Pending || update.state == order::State::Acknowledged ||
                update.state == order::State::Unknown ? 1 : 0);
            set_status("orders_partially_filled", update.state == order::State::PartiallyFilled ? 1 : 0);
            set_status("last_user_event_age_ms", 0);
        }
        if (position_changed) {
            set_status("confirmed_position_quantity", quantity);
            set_status("confirmed_entry_price", entry_price);
            set_status("confirmed_commissions", position_ledger_.commission_totals(symbol_));
            sentum::dashboard::DashboardState::global().set("active_position", quantity > 0.0
                ? nlohmann::json{{"symbol", symbol_}, {"quantity", quantity}, {"entry_price", entry_price}}
                : nlohmann::json(nullptr));
            if (exited) set_status("last_exit_reason", exit_reason);
        }
        if (reconcile_required) kill();
    }

    void initialize_position_policy() {
        persist_policy_locked(current_policy_locked(entry_price_, entry_price_,
            entry_price_ * (1.0 - risk_.stop_loss_percent), entry_price_ * (1.0 + risk_.take_profit_percent)));
    }

    using PolicyCheckpoint = sentum::risk::PositionPolicyCheckpoint;

    static std::int64_t milliseconds(std::chrono::system_clock::time_point time) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count();
    }
    PolicyCheckpoint make_policy(const order::ConfirmedPosition& position,
                                 std::chrono::system_clock::time_point entry_time,
                                 double highest, double lowest, double stop, double target) const {
        PolicyCheckpoint result;
        result.symbol = symbol_;
        result.entry_client_order_id = position.entry_client_order_id;
        result.entry_exchange_order_id = position.entry_exchange_order_id;
        result.entry_time_ms = milliseconds(entry_time);
        result.risk_policy = sentum::risk::position_policy_identity(risk_);
        result.quantity = position.quantity;
        result.entry_price = position.average_entry_price;
        result.highest_price = highest;
        result.lowest_price = lowest;
        result.stop_loss_price = stop;
        result.take_profit_price = target;
        result.updated_at_ms = milliseconds(std::chrono::system_clock::now());
        if (policy_checkpoint_ && policy_checkpoint_->same_position(result))
            result.updated_at_ms = std::max(result.updated_at_ms, policy_checkpoint_->updated_at_ms);
        result.validate();
        return result;
    }
    PolicyCheckpoint current_policy_locked(double highest, double lowest, double stop, double target) const {
        const auto position = position_ledger_.get(symbol_);
        if (!position) throw std::runtime_error("Position policy requires confirmed owned inventory");
        return make_policy(*position, entry_time_, highest, lowest, stop, target);
    }
    void publish_policy_locked(const PolicyCheckpoint& policy) {
        policy_checkpoint_ = policy;
        entry_time_ = std::chrono::system_clock::time_point(std::chrono::milliseconds(policy.entry_time_ms));
        highest_price_ = policy.highest_price;
        lowest_price_ = policy.lowest_price;
        stop_loss_price_ = policy.stop_loss_price;
        take_profit_price_ = policy.take_profit_price;
    }
    void persist_policy_locked(const PolicyCheckpoint& policy) {
        policy.validate();
        venue_->save_position_policy(symbol_, policy.to_json());
        // A tighter protection level is never published before its durable save.
        publish_policy_locked(policy);
    }
    static bool near_policy_value(double left, double right) {
        return left == right || std::abs(left - right) <=
            64 * std::numeric_limits<double>::epsilon() * std::max(std::abs(left), std::abs(right));
    }
    static bool policy_has_execution_proof(const PolicyCheckpoint& policy,
                                          const std::vector<order::Snapshot>& replay) {
        order::ConfirmedPositionLedger ledger;
        for (const auto& update : replay) {
            ledger.on_order_update(update);
            const auto position = ledger.get(policy.symbol);
            if (position && position->entry_client_order_id == policy.entry_client_order_id &&
                position->entry_exchange_order_id == policy.entry_exchange_order_id &&
                near_policy_value(position->quantity, policy.quantity) &&
                near_policy_value(position->average_entry_price, policy.entry_price)) return true;
        }
        return false;
    }
    PolicyCheckpoint recovered_policy(const ExecutionRecoveryState& recovered,
                                      const order::ConfirmedPosition& position) const {
        auto result = make_policy(position, recovered.entry_time, position.average_entry_price,
            position.average_entry_price, position.average_entry_price * (1.0 - risk_.stop_loss_percent),
            position.average_entry_price * (1.0 + risk_.take_profit_percent));
        const auto stored = venue_->load_position_policy(symbol_);
        if (!stored)
            throw std::runtime_error("Recovered position has no verified durable policy checkpoint");
        const auto saved = PolicyCheckpoint::from_json(*stored);
        if (!saved.same_position(result) || saved.risk_policy != result.risk_policy ||
            !policy_has_execution_proof(saved, recovered.replay))
            throw std::runtime_error("Recovered position policy identity or execution proof does not match");
        const auto merge_protection = [&](const PolicyCheckpoint& previous) {
            result.highest_price = std::max(result.highest_price, previous.highest_price);
            result.lowest_price = std::min(result.lowest_price, previous.lowest_price);
            result.stop_loss_price = std::max(result.stop_loss_price, previous.stop_loss_price);
            result.entry_time_ms = std::min(result.entry_time_ms, previous.entry_time_ms);
            result.updated_at_ms = std::max(result.updated_at_ms, previous.updated_at_ms);
            if (near_policy_value(result.entry_price, previous.entry_price))
                result.take_profit_price = previous.take_profit_price;
        };
        merge_protection(saved);
        if (policy_checkpoint_ && policy_checkpoint_->same_position(result) &&
            policy_checkpoint_->risk_policy == result.risk_policy) {
            if (!policy_has_execution_proof(*policy_checkpoint_, recovered.replay))
                throw std::runtime_error("Current position policy lacks reconciled execution proof");
            merge_protection(*policy_checkpoint_);
        }
        result.validate();
        return result;
    }

    void on_recovery(const ExecutionRecoveryState& recovered) {
        if (recovered.symbol != symbol_) throw std::runtime_error("Recovery belongs to another strategy market");
        std::lock_guard<std::mutex> lock(mutex_);
        if (submission_in_progress_) throw std::runtime_error("Cannot restore strategy during an active submission");
        std::unordered_map<std::string, OwnedOrder> owners;
        for (const auto& order : recovered.orders) {
            if (!order.locally_owned || order.symbol != symbol_ || order.requires_reconciliation ||
                !owners.emplace(order.client_order_id, OwnedOrder{order.side, order.exchange_order_id}).second)
                throw std::runtime_error("Recovery has invalid strategy ownership");
        }
        order::ConfirmedPositionLedger verified;
        verified.restore(recovered.replay);
        const auto position = verified.get(symbol_);
        if (position.has_value() != recovered.position.has_value() ||
            (position && (position->quantity != recovered.position->quantity ||
                          position->average_entry_price != recovered.position->average_entry_price)))
            throw std::runtime_error("Recovered strategy position differs from the account ledger");
        std::optional<PolicyCheckpoint> policy;
        if (position) {
            policy = recovered_policy(recovered, *position);
            venue_->save_position_policy(symbol_, policy->to_json());
        }
        position_ledger_.restore(recovered.replay);
        owned_orders_.swap(owners);
        active_order_.clear();
        confirmed_quantity_ = position ? position->quantity : 0.0;
        entry_price_ = position ? position->average_entry_price : 0.0;
        entry_time_ = recovered.entry_time;
        last_exit_ = recovered.last_exit;
        reconciliation_pending_ = false;
        if (policy) publish_policy_locked(*policy);
        else {
            policy_checkpoint_.reset();
            highest_price_ = lowest_price_ = stop_loss_price_ = take_profit_price_ = 0.0;
        }
        strategy_->reset();
        set_status("confirmed_position_quantity", confirmed_quantity_);
        set_status("confirmed_entry_price", entry_price_);
        set_status("confirmed_commissions", position_ledger_.commission_totals(symbol_));
        sentum::dashboard::DashboardState::global().set("active_position", confirmed_quantity_ > 0.0
            ? nlohmann::json{{"symbol", symbol_}, {"quantity", confirmed_quantity_}, {"entry_price", entry_price_}}
            : nlohmann::json(nullptr));
    }

    struct OwnedOrder {
        order::Side side;
        std::int64_t exchange_order_id = 0;
    };

    std::string symbol_;
    RiskConfig risk_;
    std::unique_ptr<IStrategy> strategy_;
    std::unique_ptr<IExecutionVenue> venue_;
    std::shared_ptr<IClock> clock_;
    RiskManager risk_manager_;
    order::OrderEventRepository events_;
    observability::StatusReporter status_;
    std::unique_ptr<BinanceWebsocketClient> price_stream_;
    std::atomic<bool> running_{false};
    std::atomic<bool> kill_requested_{false};
    std::mutex mutex_;
    bool submission_in_progress_ = false;
    bool reconciliation_pending_ = false;
    std::string active_order_;
    std::unordered_map<std::string, OwnedOrder> owned_orders_;
    order::ConfirmedPositionLedger position_ledger_;
    std::optional<PolicyCheckpoint> policy_checkpoint_;
    std::string exit_reason_;
    double confirmed_quantity_ = 0.0;
    double entry_price_ = 0.0;
    double highest_price_ = 0.0, lowest_price_ = 0.0, stop_loss_price_ = 0.0, take_profit_price_ = 0.0;
    std::chrono::system_clock::time_point entry_time_{};
    std::chrono::system_clock::time_point last_exit_{};
};

} // namespace sentum::execution
