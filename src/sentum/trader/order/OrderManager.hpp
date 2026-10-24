#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>
#include <sentum/api/BinanceSpotExecutionClient.hpp>
#include <sentum/trader/order/OrderEventRepository.hpp>
#include <sentum/trader/order/OrderTypes.hpp>

namespace sentum::order {

// The manager owns canonical order identity and durable exchange evidence. All
// observers run after the state lock is released and after persistence succeeds.
class OrderManager {
public:
    using UpdateHandler = std::function<void(const Snapshot&)>;

    explicit OrderManager(BinanceSpotExecutionClient& exchange, OrderEventRepository* repository = nullptr)
        : exchange_(exchange), repository_(repository) {
        if (repository_) kill_switch_.store(repository_->load_kill_switch());
    }

    void set_update_handler(UpdateHandler handler) {
        std::lock_guard<std::mutex> lock(mutex_);
        handler_ = std::move(handler);
    }

    void set_admission_guard(std::function<bool()> guard) {
        std::lock_guard<std::mutex> lock(mutex_);
        admission_guard_ = std::move(guard);
    }

    // Stream callbacks latch admission without synchronously issuing network
    // cancellations. The supervisor owns cancellation and transport replacement.
    void block_submissions(const std::string& reason) noexcept { latch(reason); }

    std::vector<Snapshot> snapshots() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<Snapshot> result;
        result.reserve(orders_.size());
        for (const auto& item : orders_) result.push_back(item.second);
        std::sort(result.begin(), result.end(), [](const Snapshot& a, const Snapshot& b) {
            return a.client_order_id < b.client_order_id;
        });
        return result;
    }

    void accept_account_reconciliation(const std::vector<Snapshot>& orders,
                                       const std::vector<Snapshot>& replay,
                                       const std::string& symbol, const nlohmann::json& proof) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (storage_failed_.load()) throw std::logic_error("Execution storage is unhealthy");
        std::unordered_map<std::string, Snapshot> next;
        std::set<std::string> exchange_ids;
        std::map<std::string, std::string> aliases;
        for (const auto& order : orders) {
            validate_snapshot(order);
            const auto old = orders_.find(order.client_order_id);
            if (!order.locally_owned || order.requires_reconciliation || !terminal(order.state) ||
                order.symbol != symbol || old == orders_.end() || !old->second.locally_owned ||
                old->second.symbol != order.symbol || old->second.side != order.side ||
                !near(old->second.requested_quantity, order.requested_quantity) ||
                (old->second.exchange_order_id > 0 && old->second.exchange_order_id != order.exchange_order_id) ||
                order.executed_quantity < old->second.executed_quantity ||
                order.cumulative_quote_quantity < old->second.cumulative_quote_quantity ||
                !next.emplace(order.client_order_id, order).second ||
                (order.exchange_order_id > 0 && !exchange_ids.insert(exchange_key(order)).second))
                throw std::logic_error("Account reconciliation changed order ownership or lost execution");
            const auto reserve_alias = [&](const std::string& alias) {
                const auto inserted = aliases.emplace(alias_key(order.symbol, alias), order.client_order_id);
                if (!valid_identifier(alias) || (!inserted.second && inserted.first->second != order.client_order_id))
                    throw std::logic_error("Account reconciliation contains conflicting order aliases");
            };
            reserve_alias(order.client_order_id);
            for (const auto& alias : order.client_order_aliases) reserve_alias(alias);
            for (const auto& alias : old->second.client_order_aliases)
                if (std::find(order.client_order_aliases.begin(), order.client_order_aliases.end(), alias) == order.client_order_aliases.end())
                    throw std::logic_error("Account reconciliation discarded a durable order alias");
        }
        if (next.size() != orders_.size()) throw std::logic_error("Account reconciliation omitted an order");
        try {
            if (repository_) repository_->save_reconciled_orders(orders, replay, symbol, proof);
            orders_.swap(next);
            rebuild_indexes_locked();
            reconciled_.store(true);
        } catch (...) {
            latch_locked("Account reconciliation could not be committed");
            throw;
        }
    }

    void restore_persisted_state() {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!restored_ && repository_) {
                    for (auto order : repository_->load_latest()) {
                        validate_snapshot(order);
                        if (order.state == State::Pending) {
                            order.state = State::Unknown;
                            order.rejection_reason = "Placement may have crossed dispatch before restart";
                            repository_->save_order(order, "unresolved_recovery");
                        }
                        if (!orders_.emplace(order.client_order_id, order).second)
                            throw std::runtime_error("Duplicate persisted order identity");
                    }
                    rebuild_indexes_locked();
                    kill_switch_.store(kill_switch_.load() || repository_->load_kill_switch());
                }
                restored_ = true;
    }

    void reconcile_startup() {
        reconciled_.store(false);
        try {
            restore_persisted_state();
            const auto open = exchange_.open_orders();
            if (!open.is_array()) throw std::runtime_error("Invalid open-orders reconciliation response");
            for (const auto& value : open) {
                auto incoming = from_rest(value);
                std::optional<Snapshot> update;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    update = merge_locked(incoming, {}, "reconciliation");
                }
                if (update) publish(*update);
            }
            reconcile_unresolved();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                for (const auto& item : orders_) {
                    if (!terminal(item.second.state) || item.second.requires_reconciliation) {
                        if (!kill_switch_.load()) latch_locked("Startup reconciliation contains unresolved orders");
                        break;
                    }
                }
            }
            if (!storage_failed_.load()) reconciled_.store(true);
        } catch (...) {
            latch("Startup order reconciliation failed");
            throw;
        }
    }

    // One bounded query per unresolved owned order. The session supervisor calls
    // this repeatedly; placement is never retried and NotFound is not rejection.
    void reconcile_unresolved() {
        std::vector<std::string> pending;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& item : orders_) {
                if (item.second.locally_owned && !terminal(item.second.state))
                    pending.push_back(item.first);
            }
        }
        for (const auto& id : pending) query_one(id);
    }

    Snapshot submit_market(const Request& request) {
        if (!valid_identifier(request.symbol) || !valid_identifier(request.client_order_id) ||
            (request.side != Side::Buy && request.side != Side::Sell) ||
            !std::isfinite(request.quantity) || request.quantity <= 0.0)
            throw std::invalid_argument("Invalid order request");
        Snapshot pending;
        pending.symbol = request.symbol;
        pending.client_order_id = request.client_order_id;
        pending.side = request.side;
        pending.requested_quantity = request.quantity;
        pending.locally_owned = true;
        pending.base_asset = request.base_asset;
        pending.quote_asset = request.quote_asset;
        pending.updated_at = std::chrono::system_clock::now();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!reconciled_.load()) throw std::logic_error("Orders cannot be submitted before startup reconciliation");
            if (kill_switch_.load() || storage_failed_.load()) throw std::logic_error("Kill switch is active");
            if (admission_guard_ && !admission_guard_()) throw std::logic_error("Execution stream or account proof is unavailable");
            if (orders_.count(request.client_order_id) || aliases_.count(alias_key(request.symbol, request.client_order_id)))
                throw std::logic_error("Duplicate client order id");
            commit_locked(pending, "intent");
        }
        publish(pending);

        // Local Pending observers may synchronously latch a persistence failure.
        if (kill_switch_.load()) return reject_before_dispatch(request.client_order_id, "Submission blocked by kill switch");
        if (repository_) {
            try {
                const auto metadata = exchange_.exchange_info(request.symbol);
                std::string base, quote;
                if (!metadata.is_object() || !metadata.contains("symbols") || !metadata.at("symbols").is_array())
                    throw std::runtime_error("Invalid exchange asset metadata");
                for (const auto& symbol : metadata.at("symbols")) {
                    if (symbol.value("symbol", std::string{}) == request.symbol) {
                        base = symbol.at("baseAsset").get<std::string>();
                        quote = symbol.at("quoteAsset").get<std::string>();
                    }
                }
                if (!valid_identifier(base) || !valid_identifier(quote) || base == quote)
                    throw std::runtime_error("Missing exchange asset metadata");
                std::lock_guard<std::mutex> lock(mutex_);
                auto current = orders_.at(request.client_order_id);
                if ((!current.base_asset.empty() && current.base_asset != base) ||
                    (!current.quote_asset.empty() && current.quote_asset != quote))
                    throw std::runtime_error("Order assets disagree with exchange metadata");
                current.base_asset = base;
                current.quote_asset = quote;
                commit_locked(current, "metadata");
            } catch (const std::exception& error) {
                return reject_before_dispatch(request.client_order_id, error.what());
            }
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!kill_switch_.load() && !storage_failed_.load() && (!admission_guard_ || admission_guard_()) &&
                orders_.at(request.client_order_id).state == State::Pending)
                dispatched_.insert(request.client_order_id);
        }
        if (!was_dispatched(request.client_order_id))
            return reject_before_dispatch(request.client_order_id, "Submission blocked before dispatch");

        std::optional<nlohmann::json> acknowledgement;
        std::string failure;
        bool rejected = false;
        try { acknowledgement = exchange_.place_market_order(request); }
        catch (const BinanceRequestError& error) {
            failure = error.what();
            rejected = error.kind() == BinanceRequestError::Kind::Rejected;
        } catch (const std::exception& error) { failure = error.what(); }
        catch (...) { failure = "Unknown order submission failure"; }
        if (!acknowledgement) return submission_failure(request.client_order_id, failure, rejected);

        std::optional<Snapshot> update;
        try {
            const auto incoming = from_ack(*acknowledgement, request);
            std::lock_guard<std::mutex> lock(mutex_);
            update = merge_locked(incoming, request.client_order_id, "acknowledgement");
        } catch (const std::exception& error) {
            return submission_failure(request.client_order_id, error.what(), false);
        }
        if (update) publish(*update);
        auto result = get(request.client_order_id).value();
        // A kill racing the request must also cover an ACK that arrived later.
        if (kill_switch_.load() && (result.state == State::Acknowledged || result.state == State::PartiallyFilled))
            result = cancel(result.client_order_id);
        return result;
    }

    Snapshot cancel(const std::string& id) {
        Snapshot cancelling;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = orders_.find(id);
            if (it == orders_.end()) throw std::out_of_range("Unknown client order id");
            if (terminal(it->second.state) || it->second.state == State::Cancelling) return it->second;
            // This reservation has not crossed the placement boundary yet.
            if (it->second.state == State::Pending && !dispatched_.count(id)) {
                cancelling = it->second;
                cancelling.state = State::Rejected;
                cancelling.rejection_reason = "Cancelled before submission";
                cancelling.updated_at = std::chrono::system_clock::now();
                commit_locked(cancelling, "local_cancel");
            } else {
                cancelling = it->second;
                cancelling.state = State::Cancelling;
                cancelling.updated_at = std::chrono::system_clock::now();
                commit_locked(cancelling, "cancel_intent");
            }
        }
        publish(cancelling);
        if (cancelling.state == State::Rejected) return cancelling;
        std::optional<nlohmann::json> response;
        std::string failure;
        try { response = exchange_.cancel_order(cancelling.symbol, cancelling.client_order_id); }
        catch (const std::exception& error) { failure = error.what(); }
        catch (...) { failure = "Unknown cancellation outcome"; }
        std::optional<Snapshot> update;
        if (response) {
            try {
                auto incoming = from_rest(*response);
                std::lock_guard<std::mutex> lock(mutex_);
                update = merge_locked(incoming, id, "cancellation");
            } catch (const std::exception& error) { failure = error.what(); }
        }
        if (!failure.empty()) return submission_failure(id, "Cancellation outcome unknown: " + failure, false);
        if (update) publish(*update);
        return get(id).value();
    }

    // Returns whether activation was durably recorded. Even a storage failure
    // leaves the in-process latch set; the repository's active-session guard
    // prevents a clean restart from silently accepting new orders.
    bool activate_kill_switch(const std::string& reason = "Testnet kill switch activated") noexcept {
        std::vector<std::string> cancellable;
        bool persisted = false;
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            persisted = latch_locked(reason);
            for (const auto& item : orders_) {
                const auto& order = item.second;
                if (order.locally_owned && order.exchange_order_id > 0 &&
                    (order.state == State::Acknowledged || order.state == State::PartiallyFilled))
                    cancellable.push_back(item.first);
            }
        } catch (...) { kill_switch_.store(true); storage_failed_.store(true); }
        for (const auto& id : cancellable) { try { cancel(id); } catch (...) {} }
        return persisted;
    }

    std::uint64_t kill_generation() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return kill_generation_;
    }

    void clear_kill_switch_for_reconciled_resume(std::optional<std::uint64_t> expected_generation = std::nullopt) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!reconciled_.load() || storage_failed_.load()) throw std::logic_error("Cannot resume before durable reconciliation");
        if (expected_generation && *expected_generation != kill_generation_)
            throw std::logic_error("A newer kill request invalidated the reconciled resume");
        for (const auto& item : orders_)
            if (!terminal(item.second.state) || item.second.requires_reconciliation)
                throw std::logic_error("Cannot resume with unresolved order or execution evidence");
        try {
            if (repository_) repository_->set_kill_switch(false, "Explicit reconciled Testnet resume");
        } catch (...) {
            kill_switch_.store(true);
            storage_failed_.store(true);
            throw;
        }
        kill_switch_.store(false);
    }

    bool kill_switch_active() const noexcept { return kill_switch_.load(); }
    bool reconciled() const noexcept { return reconciled_.load(); }
    bool storage_healthy() const noexcept { return !storage_failed_.load(); }
    std::optional<Snapshot> get(const std::string& id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = orders_.find(id);
        return it == orders_.end() ? std::nullopt : std::optional<Snapshot>(it->second);
    }

    void on_user_data_event(const nlohmann::json& envelope) {
        std::optional<Snapshot> update;
        try {
            const auto& event = envelope.contains("event") ? envelope.at("event") : envelope;
            if (!event.is_object() || event.value("e", std::string{}) != "executionReport") return;
            auto incoming = from_event(event);
            std::lock_guard<std::mutex> lock(mutex_);
            update = merge_locked(incoming, {}, "user_stream");
        } catch (...) {
            latch("Invalid or contradictory exchange order update");
            return;
        }
        if (update) publish(*update);
    }

private:
    struct Incoming {
        Snapshot value;
        std::string original_client_id;
        bool acknowledgement = false;
        bool execution_details = false;
        double last_executed_quantity = 0.0;
        std::int64_t execution_trade_id = -1;
    };

    static bool terminal(State state) noexcept {
        return state == State::Filled || state == State::Cancelled || state == State::Rejected;
    }
    static bool valid_identifier(const std::string& value) {
        return !value.empty() && value.size() <= 128 &&
            std::all_of(value.begin(), value.end(), [](unsigned char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
            });
    }
    static bool near(double a, double b) {
        return std::abs(a - b) <= 1e-10 * std::max({std::abs(a), std::abs(b), 1e-12});
    }
    static std::string alias_key(const std::string& symbol, const std::string& id) { return symbol + "/" + id; }
    static std::string exchange_key(const Snapshot& order) { return alias_key(order.symbol, std::to_string(order.exchange_order_id)); }
    static double number(const nlohmann::json& value, const char* key) {
        const auto& field = value.at(key);
        double result;
        if (field.is_string()) {
            const auto text = field.get<std::string>();
            std::size_t consumed = 0;
            result = std::stod(text, &consumed);
            if (consumed != text.size()) throw std::runtime_error("Invalid exchange numeric field");
        } else if (field.is_number()) result = field.get<double>();
        else throw std::runtime_error("Invalid exchange numeric type");
        if (!std::isfinite(result) || result < 0.0) throw std::runtime_error("Invalid exchange numeric value");
        return result;
    }
    static std::int64_t integer(const nlohmann::json& value, const char* key, std::int64_t fallback = 0) {
        if (!value.contains(key)) return fallback;
        if (!value.at(key).is_number_integer()) throw std::runtime_error("Invalid exchange integer field");
        const auto result = value.at(key).get<std::int64_t>();
        if (result < 0) throw std::runtime_error("Invalid negative exchange integer");
        return result;
    }
    static std::chrono::system_clock::time_point event_time(std::int64_t milliseconds) {
        const auto maximum = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::duration::max()).count();
        if (milliseconds < 0 || milliseconds > maximum)
            throw std::runtime_error("Exchange time exceeds the supported clock range");
        return std::chrono::system_clock::time_point(std::chrono::milliseconds(milliseconds));
    }
    static std::int64_t trade_id(const nlohmann::json& event) {
        if (!event.contains("t")) return -1;
        const auto& value = event.at("t");
        if (!value.is_number_integer() ||
            (value.is_number_unsigned() && value.get<std::uint64_t>() >
                static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())))
            throw std::runtime_error("Invalid exchange trade identity");
        const auto result = value.get<std::int64_t>();
        if (result < -1) throw std::runtime_error("Invalid negative exchange trade identity");
        return result;
    }
    static State state(const std::string& value) {
        if (value == "NEW") return State::Acknowledged;
        if (value == "PARTIALLY_FILLED") return State::PartiallyFilled;
        if (value == "FILLED") return State::Filled;
        if (value == "PENDING_CANCEL") return State::Cancelling;
        if (value == "CANCELED" || value == "EXPIRED" || value == "EXPIRED_IN_MATCH") return State::Cancelled;
        if (value == "REJECTED") return State::Rejected;
        throw std::runtime_error("Unknown exchange order status");
    }
    static Side side(const std::string& value) {
        if (value == "BUY") return Side::Buy;
        if (value == "SELL") return Side::Sell;
        throw std::runtime_error("Invalid exchange order side");
    }
    static void validate_snapshot(const Snapshot& order) {
        if (!valid_identifier(order.symbol) || !valid_identifier(order.client_order_id) ||
            !std::isfinite(order.requested_quantity) || order.requested_quantity <= 0.0 ||
            !std::isfinite(order.executed_quantity) || order.executed_quantity < 0.0 ||
            !std::isfinite(order.cumulative_quote_quantity) || order.cumulative_quote_quantity < 0.0 ||
            order.exchange_order_id < 0 || order.last_event_time_ms < 0 ||
            (order.executed_quantity > order.requested_quantity && !near(order.executed_quantity, order.requested_quantity)))
            throw std::runtime_error("Invalid order snapshot");
        if ((order.executed_quantity == 0.0) != (order.cumulative_quote_quantity == 0.0))
            throw std::runtime_error("Inconsistent cumulative execution");
        if (order.executed_quantity == 0.0 && !order.commissions.empty())
            throw std::runtime_error("Commission has no cumulative execution");
        if (order.executed_quantity > 0.0 && (!std::isfinite(order.average_fill_price) || order.average_fill_price <= 0.0))
            throw std::runtime_error("Invalid average execution price");
        if (order.state == State::Filled && !near(order.executed_quantity, order.requested_quantity))
            throw std::runtime_error("Filled order has incomplete cumulative execution");
        for (const auto& fee : order.commissions)
            if (fee.first < 0 || !std::isfinite(fee.second.amount) || fee.second.amount < 0.0 ||
                (fee.second.amount > 0.0 && !valid_identifier(fee.second.asset)))
                throw std::runtime_error("Invalid execution commission");
        long double known_quantity = 0.0;
        for (const auto& trade : order.execution_trade_quantities) {
            if (trade.first < 0 || !std::isfinite(trade.second) || trade.second <= 0.0)
                throw std::runtime_error("Invalid execution trade quantity");
            known_quantity += trade.second;
        }
        const long double rounding = 8.0L * std::numeric_limits<double>::epsilon() *
            static_cast<long double>(std::max<std::size_t>(1, order.execution_trade_quantities.size())) *
            static_cast<long double>(order.executed_quantity);
        if (known_quantity > static_cast<long double>(order.executed_quantity) + rounding)
            throw std::runtime_error("Known trade quantities exceed cumulative execution");
    }
    static Incoming from_event(const nlohmann::json& event) {
        Incoming incoming;
        auto& order = incoming.value;
        order.symbol = event.at("s").get<std::string>();
        order.client_order_id = event.at("c").get<std::string>();
        incoming.original_client_id = event.value("C", std::string{});
        order.exchange_order_id = integer(event, "i");
        if (order.exchange_order_id <= 0) throw std::runtime_error("Missing exchange order identity");
        order.side = side(event.at("S").get<std::string>());
        order.requested_quantity = number(event, "q");
        order.executed_quantity = number(event, "z");
        order.cumulative_quote_quantity = number(event, "Z");
        order.average_fill_price = order.executed_quantity > 0.0 ? order.cumulative_quote_quantity / order.executed_quantity : 0.0;
        order.state = state(event.at("X").get<std::string>());
        order.rejection_reason = event.value("r", std::string{});
        order.last_event_time_ms = integer(event, "T", integer(event, "E"));
        order.updated_at = event_time(order.last_event_time_ms);
        if (event.contains("n")) {
            const double amount = number(event, "n");
            const auto trade = trade_id(event);
            std::string asset;
            if (event.contains("N") && !event.at("N").is_null()) asset = event.at("N").get<std::string>();
            if (amount > 0.0 && (trade < 0 || !valid_identifier(asset)))
                throw std::runtime_error("Commission has no trade or asset identity");
            if (trade >= 0) order.commissions.emplace(trade, ExecutionCommission{asset, amount});
            if (trade >= 0 && event.contains("l")) {
                incoming.last_executed_quantity = number(event, "l");
                incoming.execution_details = incoming.last_executed_quantity > 0.0;
                incoming.execution_trade_id = trade;
                if (incoming.execution_details)
                    order.execution_trade_quantities.emplace(trade, incoming.last_executed_quantity);
            }
        }
        validate_snapshot(order);
        return incoming;
    }
    static Incoming from_rest(const nlohmann::json& value) {
        Incoming incoming;
        auto& order = incoming.value;
        order.symbol = value.at("symbol").get<std::string>();
        order.client_order_id = value.at("clientOrderId").get<std::string>();
        incoming.original_client_id = value.value("origClientOrderId", std::string{});
        order.exchange_order_id = integer(value, "orderId");
        if (order.exchange_order_id <= 0) throw std::runtime_error("Missing exchange order identity");
        order.side = side(value.at("side").get<std::string>());
        order.requested_quantity = number(value, "origQty");
        order.executed_quantity = number(value, "executedQty");
        order.cumulative_quote_quantity = number(value, "cummulativeQuoteQty");
        order.average_fill_price = order.executed_quantity > 0.0 ? order.cumulative_quote_quantity / order.executed_quantity : 0.0;
        order.state = state(value.at("status").get<std::string>());
        order.last_event_time_ms = integer(value, "updateTime", integer(value, "transactTime"));
        order.updated_at = order.last_event_time_ms > 0 ?
            event_time(order.last_event_time_ms) :
            std::chrono::system_clock::now();
        validate_snapshot(order);
        return incoming;
    }
    static Incoming from_ack(const nlohmann::json& value, const Request& request) {
        if (!value.is_object()) throw std::runtime_error("Invalid placement acknowledgement");
        Incoming incoming;
        auto& order = incoming.value;
        order.symbol = value.value("symbol", request.symbol);
        order.client_order_id = value.value("clientOrderId", request.client_order_id);
        order.side = request.side;
        order.requested_quantity = request.quantity;
        order.exchange_order_id = integer(value, "orderId");
        if (order.exchange_order_id <= 0) throw std::runtime_error("Placement acknowledgement has no exchange identity");
        order.state = State::Acknowledged;
        order.updated_at = std::chrono::system_clock::now();
        incoming.acknowledgement = true;
        validate_snapshot(order);
        return incoming;
    }

    std::optional<Snapshot> merge_locked(const Incoming& incoming, const std::string& expected, const std::string& source) {
        const auto& event = incoming.value;
        if (!incoming.original_client_id.empty() && !valid_identifier(incoming.original_client_id))
            throw std::runtime_error("Invalid original client order identity");
        std::set<std::string> matches;
        const auto match_alias = [&](const std::string& alias) {
            if (alias.empty()) return;
            const auto direct = orders_.find(alias);
            if (direct != orders_.end()) {
                if (direct->second.symbol != event.symbol) throw std::runtime_error("Order symbol identity conflict");
                matches.insert(direct->first);
            }
            const auto known = aliases_.find(alias_key(event.symbol, alias));
            if (known != aliases_.end()) matches.insert(known->second);
        };
        match_alias(event.client_order_id);
        match_alias(incoming.original_client_id);
        const auto exchange_id = exchange_ids_.find(exchange_key(event));
        if (exchange_id != exchange_ids_.end()) matches.insert(exchange_id->second);
        if (!expected.empty()) {
            if (!orders_.count(expected)) throw std::runtime_error("Unknown expected order");
            matches.insert(expected);
        }
        if (matches.size() > 1) throw std::runtime_error("Conflicting order aliases");
        if (matches.empty()) {
            Snapshot external = event;
            if (!incoming.original_client_id.empty()) {
                external.client_order_id = incoming.original_client_id;
                external.client_order_aliases.push_back(event.client_order_id);
            }
            external.locally_owned = false;
            external.requires_reconciliation = true;
            commit_locked(external, source);
            latch_locked("Unowned account order requires reconciliation");
            return external;
        }
        auto current = orders_.at(*matches.begin());
        if (current.symbol != event.symbol || current.side != event.side ||
            !near(current.requested_quantity, event.requested_quantity) ||
            (current.exchange_order_id > 0 && current.exchange_order_id != event.exchange_order_id))
            throw std::runtime_error("Conflicting stable order identity");
        if (!expected.empty() && current.exchange_order_id == 0 &&
            event.client_order_id != current.client_order_id && incoming.original_client_id != current.client_order_id)
            throw std::runtime_error("Response did not confirm the requested client identity");
        bool changed = current.exchange_order_id == 0;
        current.exchange_order_id = event.exchange_order_id;
        const auto add_alias = [&](const std::string& alias) {
            if (!alias.empty() && alias != current.client_order_id &&
                std::find(current.client_order_aliases.begin(), current.client_order_aliases.end(), alias) == current.client_order_aliases.end()) {
                current.client_order_aliases.push_back(alias);
                changed = true;
            }
        };
        add_alias(event.client_order_id);
        add_alias(incoming.original_client_id);
        const bool new_trade = incoming.execution_details &&
            !current.execution_trade_quantities.count(incoming.execution_trade_id);
        if (incoming.execution_details && !new_trade) {
            const auto prior_fee = current.commissions.find(incoming.execution_trade_id);
            const auto event_fee = event.commissions.find(incoming.execution_trade_id);
            const double prior_amount = prior_fee == current.commissions.end() ? 0.0 : prior_fee->second.amount;
            const double event_amount = event_fee == event.commissions.end() ? 0.0 : event_fee->second.amount;
            if (prior_amount != event_amount)
                throw std::runtime_error("Commission changed for a previously identified trade");
        }
        for (const auto& trade : event.execution_trade_quantities) {
            const auto known = current.execution_trade_quantities.find(trade.first);
            if (known != current.execution_trade_quantities.end()) {
                if (known->second != trade.second) throw std::runtime_error("Exchange trade quantity changed");
            } else { current.execution_trade_quantities.emplace(trade); changed = true; }
        }
        for (const auto& fee : event.commissions) {
            const auto existing = current.commissions.find(fee.first);
            if (existing != current.commissions.end()) {
                if (existing->second.asset != fee.second.asset || !near(existing->second.amount, fee.second.amount))
                    throw std::runtime_error("Conflicting commission for exchange trade");
            } else { current.commissions.emplace(fee); changed = true; }
            if (!current.execution_trade_quantities.count(fee.first)) {
                current.requires_reconciliation = true;
                latch_locked("Commission lacks a unique execution trade quantity");
                changed = true;
            }
        }
        const bool stale_quantity = event.executed_quantity < current.executed_quantity;
        const bool stale_quote = event.cumulative_quote_quantity < current.cumulative_quote_quantity;
        const bool stale_time = event.last_event_time_ms > 0 && current.last_event_time_ms > 0 &&
                                event.last_event_time_ms < current.last_event_time_ms;
        if (!incoming.acknowledgement) {
            const int quantity_direction = event.executed_quantity == current.executed_quantity ? 0 :
                (event.executed_quantity > current.executed_quantity ? 1 : -1);
            const int quote_direction = event.cumulative_quote_quantity == current.cumulative_quote_quantity ? 0 :
                (event.cumulative_quote_quantity > current.cumulative_quote_quantity ? 1 : -1);
            if (quantity_direction != quote_direction)
                throw std::runtime_error("Cumulative quantity and quote changed inconsistently");
            if (stale_time && terminal(event.state) && event.state != current.state)
                latch_locked("Stale contradictory terminal order report");
        }
        if (!incoming.acknowledgement && !stale_quantity && !stale_quote) {
            const bool increased = event.executed_quantity > current.executed_quantity;
            if (increased) {
                const double delta = event.executed_quantity - current.executed_quantity;
                const double rounding = 4.0 * std::numeric_limits<double>::epsilon() *
                    std::max(event.executed_quantity, current.executed_quantity);
                const bool complete = incoming.execution_details && new_trade &&
                    std::abs(delta - incoming.last_executed_quantity) <= std::max(rounding, delta * 1e-10);
                if (!complete) {
                    current.requires_reconciliation = true;
                    latch_locked("Cumulative execution lacks complete per-trade commission evidence");
                    changed = true;
                }
            }
            if (increased && terminal(current.state)) latch_locked("Execution increased after a terminal order report");
            if (current.executed_quantity != event.executed_quantity || current.cumulative_quote_quantity != event.cumulative_quote_quantity) changed = true;
            current.executed_quantity = std::max(current.executed_quantity, event.executed_quantity);
            current.cumulative_quote_quantity = std::max(current.cumulative_quote_quantity, event.cumulative_quote_quantity);
            current.average_fill_price = current.executed_quantity > 0.0 ? current.cumulative_quote_quantity / current.executed_quantity : 0.0;
            State next = current.state;
            if (current.state == State::Filled) next = State::Filled;
            else if (terminal(current.state)) {
                if (event.state == State::Filled && increased) next = State::Filled;
            } else if (stale_time && !increased) next = current.state;
            else if (event.state == State::Filled || event.state == State::Cancelled || event.state == State::Rejected) next = event.state;
            else if (current.state == State::Cancelling) next = State::Cancelling;
            else if (current.executed_quantity > 0.0) next = State::PartiallyFilled;
            else next = event.state;
            if (next != current.state) { current.state = next; changed = true; }
            if (current.state == State::Rejected && current.rejection_reason != event.rejection_reason) {
                current.rejection_reason = event.rejection_reason;
                changed = true;
            }
        } else if (incoming.acknowledgement && (current.state == State::Pending || current.state == State::Unknown)) {
            current.state = current.executed_quantity > 0.0 ? State::PartiallyFilled : State::Acknowledged;
            changed = true;
        }
        if (!changed) return std::nullopt;
        current.last_event_time_ms = std::max(current.last_event_time_ms, event.last_event_time_ms);
        current.updated_at = std::max(current.updated_at, event.updated_at);
        validate_snapshot(current);
        commit_locked(current, source);
        return current;
    }

    void commit_locked(const Snapshot& snapshot, const std::string& source) {
        try { if (repository_) repository_->save_order(snapshot, source); }
        catch (...) {
            storage_failed_.store(true);
            reconciled_.store(false);
            latch_locked("Order evidence persistence failed");
            throw;
        }
        orders_[snapshot.client_order_id] = snapshot;
        rebuild_indexes_locked();
    }
    void rebuild_indexes_locked() {
        aliases_.clear();
        exchange_ids_.clear();
        for (const auto& item : orders_) {
            const auto& value = item.second;
            const auto insert = [&](auto& index, const std::string& key) {
                const auto result = index.emplace(key, item.first);
                if (!result.second && result.first->second != item.first)
                    throw std::runtime_error("Duplicate canonical order index");
            };
            insert(aliases_, alias_key(value.symbol, value.client_order_id));
            for (const auto& alias : value.client_order_aliases) insert(aliases_, alias_key(value.symbol, alias));
            if (value.exchange_order_id > 0) insert(exchange_ids_, exchange_key(value));
        }
    }
    bool latch_locked(const std::string& reason) noexcept {
        kill_switch_.store(true);
        if (kill_generation_ == std::numeric_limits<std::uint64_t>::max()) {
            storage_failed_.store(true);
            reconciled_.store(false);
            return false;
        }
        ++kill_generation_;
        try {
            if (repository_) repository_->set_kill_switch(true, reason);
            return !storage_failed_.load();
        } catch (...) {
            storage_failed_.store(true);
            reconciled_.store(false);
            return false;
        }
    }
    void latch(const std::string& reason) noexcept {
        try { std::lock_guard<std::mutex> lock(mutex_); latch_locked(reason); }
        catch (...) { kill_switch_.store(true); storage_failed_.store(true); }
    }
    bool was_dispatched(const std::string& id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return dispatched_.count(id) != 0;
    }
    Snapshot reject_before_dispatch(const std::string& id, const std::string& reason) {
        Snapshot result;
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            result = orders_.at(id);
            if (result.state == State::Pending && !dispatched_.count(id)) {
                result.state = State::Rejected;
                result.rejection_reason = reason;
                result.updated_at = std::chrono::system_clock::now();
                commit_locked(result, "not_submitted");
                changed = true;
            }
        }
        if (changed) publish(result);
        return result;
    }
    Snapshot submission_failure(const std::string& id, const std::string& reason, bool explicit_rejection) {
        Snapshot result;
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            result = orders_.at(id);
            if (!terminal(result.state)) {
                const bool definitely_rejected = explicit_rejection && result.state == State::Pending && result.exchange_order_id == 0;
                result.state = definitely_rejected ? State::Rejected : State::Unknown;
                result.rejection_reason = reason;
                result.updated_at = std::chrono::system_clock::now();
                commit_locked(result, definitely_rejected ? "rejected" : "unknown_outcome");
                changed = true;
                if (!definitely_rejected) latch_locked("Unknown exchange order outcome");
            } else if (!explicit_rejection) {
                latch_locked("Contradictory or unknown response after a confirmed terminal order");
            }
        }
        if (changed) publish(result);
        if (result.state == State::Unknown) query_one(id);
        return get(id).value();
    }
    void query_one(const std::string& id) {
        const auto previous = get(id);
        if (!previous || terminal(previous->state) || !previous->locally_owned) return;
        std::optional<Snapshot> update;
        try {
            const auto response = exchange_.query_order(previous->symbol, previous->client_order_id);
            const auto incoming = from_rest(response);
            std::lock_guard<std::mutex> lock(mutex_);
            update = merge_locked(incoming, previous->client_order_id, "order_query");
        } catch (...) {
            // A transport failure or NotFound can lag the matching engine. The
            // durable unresolved identity remains authoritative until a query or
            // stream event supplies a complete exchange result.
            std::lock_guard<std::mutex> lock(mutex_);
            auto current = orders_.at(id);
            if (!terminal(current.state)) {
                latch_locked("Order query has not established an authoritative outcome");
                if (current.state == State::Pending) {
                    current.state = State::Unknown;
                    current.rejection_reason = "Unresolved persisted placement";
                    try { commit_locked(current, "unresolved_recovery"); update = current; } catch (...) {}
                }
            }
        }
        if (update) publish(*update);
    }
    void publish(const Snapshot& update) {
        UpdateHandler handler;
        { std::lock_guard<std::mutex> lock(mutex_); handler = handler_; }
        if (handler) handler(update);
    }

    BinanceSpotExecutionClient& exchange_;
    OrderEventRepository* repository_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Snapshot> orders_;
    std::unordered_map<std::string, std::string> aliases_, exchange_ids_;
    // Remains set after the network call: Pending is never locally rejectable
    // once placement may have crossed the transport boundary. Restored Pending
    // records are converted to Unknown before any recovery callback can cancel.
    std::set<std::string> dispatched_;
    UpdateHandler handler_;
    std::function<bool()> admission_guard_;
    bool restored_ = false;
    std::uint64_t kill_generation_ = 0;
    std::atomic<bool> reconciled_{false}, kill_switch_{false}, storage_failed_{false};
};

} // namespace sentum::order
