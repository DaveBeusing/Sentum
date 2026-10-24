#pragma once

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <sentum/api/BinanceSpotExecutionClient.hpp>
#include <sentum/operations/OperationalEventRepository.hpp>
#include <sentum/trader/order/ConfirmedPositionLedger.hpp>

namespace sentum::execution {

struct ReconciliationReport {
    bool success = false;
    int open_orders = 0;
    int balances_checked = 0;
    int inconsistencies = 0;
    std::string details;
};

struct AccountRecovery {
    ReconciliationReport report;
    std::vector<order::Snapshot> orders;
    // Exchange trade order, with cumulative quantities and explicit fee proof.
    // Rebuild a fresh ledger from this sequence before accepting live deltas.
    std::vector<order::Snapshot> replay;
    std::optional<order::ConfirmedPosition> position;
    std::chrono::system_clock::time_point entry_time{};
    std::chrono::system_clock::time_point last_exit{};
    std::set<std::string> account_permissions;
    std::optional<std::int64_t> account_uid;
    std::int64_t started_ms = 0;
    std::int64_t finished_ms = 0;
};

struct AccountRecoveryLimits {
    int page_size = 1000;
    std::size_t max_pages = 64;
    std::size_t max_replay_entries = 262144;
    std::chrono::milliseconds timeout{30000};
};

// REST snapshots are not atomic. The caller must hold submissions closed and
// reject this proof if its authenticated stream generation or received-event
// counter changes while the proof is collected and committed.
class AccountReconciler {
public:
    explicit AccountReconciler(BinanceSpotExecutionClient& client,
                               operations::OperationalEventRepository* events = nullptr)
        : client_(client), events_(events) {}
    AccountReconciler(BinanceSpotExecutionClient& client, operations::OperationalEventRepository& events)
        : AccountReconciler(client, &events) {}

    ReconciliationReport run(const std::string& symbol, double expected_base_quantity,
                             const std::string& base_asset) {
        const auto started = now_ms();
        ReconciliationReport result;
        try {
            require_identifier(symbol, "symbol");
            require_identifier(base_asset, "base asset");
            if (!std::isfinite(expected_base_quantity) || expected_base_quantity < 0.0)
                throw std::runtime_error("Invalid expected account inventory");
            result.open_orders = scoped_order_count(parse_open_orders(client_.open_orders()), symbol);
            const auto account = parse_account(client_.account());
            result.balances_checked = static_cast<int>(account.balances.size());
            const auto& balance = required_balance(account, base_asset);
            if (!near(balance.first + balance.second, expected_base_quantity))
                throw std::runtime_error("Balance mismatch for " + base_asset);
            if (result.open_orders > 1)
                throw std::runtime_error("More than one open order for the requested single-position market");
            result.success = true;
            result.details = "Account inventory and scoped open orders are consistent";
        } catch (const std::exception& error) {
            result.inconsistencies = 1;
            result.details = error.what();
        }
        record(started, result);
        return result;
    }

    AccountRecovery recover(const std::string& symbol, const std::string& base_asset,
                            const std::string& quote_asset,
                            const std::vector<order::Snapshot>& durable_orders,
                            AccountRecoveryLimits limits = {}) {
        const auto started = now_ms();
        AccountRecovery result;
        result.started_ms = started;
        try {
            require_identifier(symbol, "symbol");
            require_identifier(base_asset, "base asset");
            require_identifier(quote_asset, "quote asset");
            if (base_asset == quote_asset)
                throw std::runtime_error("Recovery requires distinct base and quote assets");
            if (limits.page_size < 1 || limits.page_size > 1000 || limits.max_pages == 0 ||
                limits.max_pages > 1024 || limits.max_replay_entries == 0 || limits.max_replay_entries > 1048576 ||
                limits.timeout.count() <= 0 || limits.timeout > std::chrono::hours(1))
                throw std::runtime_error("Invalid account recovery limits");
            const auto deadline = std::chrono::steady_clock::now() + limits.timeout;
            const auto request = [&](auto operation) {
                check_deadline(deadline);
                auto response = operation();
                check_deadline(deadline);
                return response;
            };
            const auto before = parse_account(request([&] { return client_.account(); }));
            required_balance(before, base_asset);
            required_balance(before, quote_asset);
            result.report.balances_checked = static_cast<int>(before.balances.size());
            result.account_permissions = before.permissions;
            result.account_uid = before.uid;
            const auto open_before = parse_open_orders(request([&] { return client_.open_orders(); }));
            result.report.open_orders = scoped_order_count(open_before, symbol);
            if (result.report.open_orders != 0)
                throw std::runtime_error("Open orders must be resolved before account recovery can authorize resume");

            std::set<std::string> clients;
            std::map<std::int64_t, std::size_t> owners;
            std::vector<order::Snapshot> prior;
            for (const auto& durable : durable_orders) {
                if (durable.symbol != symbol) continue;
                validate_durable(durable, base_asset, quote_asset);
                if (!clients.insert(durable.client_order_id).second)
                    throw std::runtime_error("Duplicate durable client order identity");
                auto current = durable;
                current.base_asset = base_asset;
                current.quote_asset = quote_asset;
                // Pre-dispatch/explicit rejections have no exchange execution.
                // An unresolved intent is never promoted to local rejection.
                if (durable.state == order::State::Rejected && durable.exchange_order_id == 0) {
                    if (durable.executed_quantity != 0.0 || !durable.commissions.empty() ||
                        !durable.execution_trade_quantities.empty() || durable.requires_reconciliation)
                        throw std::runtime_error("Local rejection contains unresolved execution evidence");
                } else {
                    current = verify_query(request([&] {
                        return client_.query_order(symbol, durable.client_order_id);
                    }), current);
                    if (!terminal(current.state))
                        throw std::runtime_error("Order query has not established a terminal outcome");
                    if (!owners.emplace(current.exchange_order_id, prior.size()).second)
                        throw std::runtime_error("Duplicate owned exchange order identity");
                }
                prior.push_back(durable);
                result.orders.push_back(std::move(current));
            }

            std::vector<Trade> trades;
            std::int64_t cursor = 0, previous_time = 0;
            bool complete = false;
            for (std::size_t page = 0; page < limits.max_pages; ++page) {
                const auto response = request([&] { return client_.account_trades(symbol, cursor, limits.page_size); });
                if (!response.is_array() || response.size() > static_cast<std::size_t>(limits.page_size))
                    throw std::runtime_error("Invalid account trade page");
                for (const auto& row : response) {
                    check_deadline(deadline);
                    auto trade = parse_trade(row, symbol);
                    if (trade.id < cursor || trade.time < previous_time)
                        throw std::runtime_error("Account trade history is duplicated or out of order");
                    if (trade.id == std::numeric_limits<std::int64_t>::max())
                        throw std::runtime_error("Account trade history cursor overflow");
                    cursor = trade.id + 1;
                    previous_time = trade.time;
                    const auto owner = owners.find(trade.order_id);
                    if (owner == owners.end() || result.orders[owner->second].side != trade.side)
                        throw std::runtime_error("Unowned or contradictory account trade requires explicit reconciliation");
                    if (trade.time > result.orders[owner->second].last_event_time_ms)
                        throw std::runtime_error("Trade execution is newer than its terminal order evidence");
                    trades.push_back(std::move(trade));
                }
                if (response.size() < static_cast<std::size_t>(limits.page_size)) {
                    complete = true;
                    break;
                }
            }
            if (!complete)
                throw std::runtime_error("Incomplete account trade history: recovery page limit exceeded");

            std::vector<order::Snapshot> cumulative = result.orders;
            for (auto& snapshot : cumulative) {
                snapshot.executed_quantity = 0.0;
                snapshot.cumulative_quote_quantity = 0.0;
                snapshot.average_fill_price = 0.0;
                snapshot.commissions.clear();
                snapshot.execution_trade_quantities.clear();
                snapshot.requires_reconciliation = false;
            }
            std::map<std::int64_t, std::size_t> remaining_trades;
            for (const auto& trade : trades) ++remaining_trades[trade.order_id];
            order::ConfirmedPositionLedger ledger;
            std::size_t replay_entries = 0;
            for (const auto& trade : trades) {
                check_deadline(deadline);
                auto& snapshot = cumulative.at(owners.at(trade.order_id));
                snapshot.executed_quantity += trade.quantity;
                snapshot.cumulative_quote_quantity += trade.quote;
                if (!std::isfinite(snapshot.executed_quantity) || !std::isfinite(snapshot.cumulative_quote_quantity))
                    throw std::runtime_error("Account trade totals overflowed");
                if (--remaining_trades.at(trade.order_id) == 0) {
                    const auto& summary = result.orders.at(owners.at(trade.order_id));
                    if (!near(snapshot.executed_quantity, summary.executed_quantity) ||
                        !near(snapshot.cumulative_quote_quantity, summary.cumulative_quote_quantity))
                        throw std::runtime_error("Trade history does not completely explain the exchange order totals");
                    // Preserve exchange cumulative watermarks exactly, after
                    // checking the independent per-trade decimal sums. This
                    // avoids a rounding-only delta on a duplicate live report.
                    snapshot.executed_quantity = summary.executed_quantity;
                    snapshot.cumulative_quote_quantity = summary.cumulative_quote_quantity;
                }
                snapshot.average_fill_price = snapshot.cumulative_quote_quantity / snapshot.executed_quantity;
                snapshot.commissions.emplace(trade.id, order::ExecutionCommission{trade.commission_asset, trade.commission});
                snapshot.execution_trade_quantities.emplace(trade.id, trade.quantity);
                const auto entries = snapshot.commissions.size() + snapshot.execution_trade_quantities.size();
                if (entries > limits.max_replay_entries - replay_entries)
                    throw std::runtime_error("Incomplete account recovery: retained replay evidence limit exceeded");
                replay_entries += entries;
                snapshot.updated_at = time_point(trade.time);
                snapshot.last_event_time_ms = trade.time;
                snapshot.state = order::State::PartiallyFilled;
                const auto previous = ledger.get(symbol);
                ledger.on_order_update(snapshot);
                const auto current = ledger.get(symbol);
                if (!previous && current) result.entry_time = snapshot.updated_at;
                if (previous && !current) {
                    result.entry_time = {};
                    result.last_exit = snapshot.updated_at;
                }
                result.replay.push_back(snapshot);
            }
            for (std::size_t i = 0; i < result.orders.size(); ++i) {
                check_deadline(deadline);
                auto& summary = result.orders[i];
                const auto& verified = cumulative[i];
                if (!near(verified.executed_quantity, summary.executed_quantity) ||
                    !near(verified.cumulative_quote_quantity, summary.cumulative_quote_quantity))
                    throw std::runtime_error("Trade history does not completely explain the exchange order totals");
                for (const auto& [id, quantity] : prior[i].execution_trade_quantities) {
                    const auto seen = verified.execution_trade_quantities.find(id);
                    if (seen == verified.execution_trade_quantities.end() || seen->second != quantity)
                        throw std::runtime_error("Persisted execution trade is missing or changed in exchange history");
                }
                for (const auto& [id, fee] : prior[i].commissions) {
                    const auto seen = verified.commissions.find(id);
                    if (seen == verified.commissions.end() || seen->second.amount != fee.amount ||
                        (seen->second.asset != fee.asset && !(fee.amount == 0.0 && fee.asset.empty())))
                        throw std::runtime_error("Persisted execution commission is missing or changed in exchange history");
                }
                summary.execution_trade_quantities = verified.execution_trade_quantities;
                summary.commissions = verified.commissions;
                summary.requires_reconciliation = false;
                if (summary.exchange_order_id > 0) {
                    const auto second = verify_query(request([&] {
                        return client_.query_order(symbol, summary.client_order_id);
                    }), summary);
                    if (!same_order(summary, second))
                        throw std::runtime_error("Exchange order changed during account recovery");
                }
            }
            const auto open_after = parse_open_orders(request([&] { return client_.open_orders(); }));
            const auto after = parse_account(request([&] { return client_.account(); }));
            if (open_before != open_after || !(before == after))
                throw std::runtime_error("Account or open orders changed during recovery");
            const auto tail = request([&] { return client_.account_trades(symbol, cursor, limits.page_size); });
            if (!tail.is_array() || !tail.empty())
                throw std::runtime_error("Account trade history changed during recovery");
            result.position = ledger.get(symbol);
            const auto& balance = required_balance(after, base_asset);
            if (!near(balance.first + balance.second, result.position ? result.position->quantity : 0.0))
                throw std::runtime_error("Unexplained base inventory or incomplete owned trade history");
            if (balance.second != 0.0)
                throw std::runtime_error("Locked base inventory is outside the recovered strategy orders");
            result.report.success = true;
            result.report.details = "Verified owned trades, commissions, terminal orders and account inventory for " + symbol;
        } catch (const std::exception& error) {
            result.report.success = false;
            result.report.inconsistencies = 1;
            result.report.details = error.what();
            // Partial reconstruction cannot be used as authoritative restoration.
            result.orders.clear();
            result.replay.clear();
            result.position.reset();
            result.entry_time = {};
            result.last_exit = {};
            result.account_permissions.clear();
            result.account_uid.reset();
        }
        result.finished_ms = now_ms();
        record(started, result.report);
        return result;
    }

private:
    struct Account {
        std::int64_t update_time = 0;
        std::optional<std::int64_t> uid;
        std::map<std::string, std::pair<double, double>> balances;
        std::set<std::string> permissions;
        bool operator==(const Account& other) const {
            return update_time == other.update_time && uid == other.uid &&
                balances == other.balances && permissions == other.permissions;
        }
    };
    struct Trade {
        std::int64_t id = 0, order_id = 0, time = 0;
        order::Side side = order::Side::Buy;
        double quantity = 0.0, quote = 0.0, commission = 0.0;
        std::string commission_asset;
    };
    using OpenOrders = std::map<std::pair<std::string, std::int64_t>, nlohmann::json>;

    static void require_identifier(const std::string& value, const char* kind) {
        if (value.empty() || value.size() > 128 ||
            !std::all_of(value.begin(), value.end(), [](unsigned char c) {
                return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == ':';
            })) throw std::runtime_error(std::string("Invalid ") + kind);
    }
    static std::string identifier(const nlohmann::json& value, const char* field) {
        if (!value.at(field).is_string()) throw std::runtime_error(std::string("Invalid string field: ") + field);
        auto text = value.at(field).get<std::string>();
        require_identifier(text, field);
        return text;
    }
    static std::int64_t integer(const nlohmann::json& value, const char* field, bool positive = false) {
        const auto& number = value.at(field);
        if (!number.is_number_integer() ||
            (number.is_number_unsigned() && number.get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())))
            throw std::runtime_error(std::string("Invalid integer field: ") + field);
        const auto result = number.get<std::int64_t>();
        if (result < (positive ? 1 : 0)) throw std::runtime_error(std::string("Invalid integer value: ") + field);
        return result;
    }
    static double decimal(const nlohmann::json& value, const char* field, bool positive = false) {
        const auto& input = value.at(field);
        if (!input.is_string()) throw std::runtime_error(std::string("Invalid decimal type: ") + field);
        const auto text = input.get<std::string>();
        bool dot = false, digit = false;
        for (const char c : text) {
            if (c >= '0' && c <= '9') digit = true;
            else if (c == '.' && !dot) dot = true;
            else throw std::runtime_error(std::string("Invalid decimal syntax: ") + field);
        }
        if (!digit) throw std::runtime_error(std::string("Missing decimal value: ") + field);
        double result = 0.0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result, std::chars_format::fixed);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
            !std::isfinite(result) || result < 0.0 || (positive && result == 0.0))
            throw std::runtime_error(std::string("Invalid decimal value: ") + field);
        return result;
    }
    static bool near(double a, double b) {
        if (!std::isfinite(a) || !std::isfinite(b)) return false;
        return a == b || std::abs(a - b) <= 1e-10 * std::max(std::abs(a), std::abs(b));
    }
    static bool terminal(order::State state) {
        return state == order::State::Filled || state == order::State::Cancelled || state == order::State::Rejected;
    }
    static order::State state(const std::string& value) {
        if (value == "NEW" || value == "PENDING_NEW") return order::State::Acknowledged;
        if (value == "PARTIALLY_FILLED") return order::State::PartiallyFilled;
        if (value == "FILLED") return order::State::Filled;
        if (value == "CANCELED" || value == "EXPIRED" || value == "EXPIRED_IN_MATCH") return order::State::Cancelled;
        if (value == "REJECTED") return order::State::Rejected;
        throw std::runtime_error("Unknown exchange order status");
    }
    static std::chrono::system_clock::time_point time_point(std::int64_t milliseconds) {
        const auto maximum = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::duration::max()).count();
        if (milliseconds <= 0 || milliseconds > maximum) throw std::runtime_error("Execution timestamp is out of range");
        return std::chrono::system_clock::time_point(std::chrono::milliseconds(milliseconds));
    }
    static Account parse_account(const nlohmann::json& value) {
        if (!value.is_object() || !value.at("canTrade").is_boolean() || !value.at("canTrade").get<bool>() ||
            !value.at("accountType").is_string() || value.at("accountType") != "SPOT" ||
            !value.at("balances").is_array() || value.at("balances").empty() ||
            !value.at("permissions").is_array() || value.at("permissions").empty())
            throw std::runtime_error("Missing or invalid tradable Spot account snapshot");
        Account result;
        result.update_time = integer(value, "updateTime");
        if (result.update_time > std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::duration::max()).count())
            throw std::runtime_error("Account update timestamp is out of range");
        if (value.contains("uid")) result.uid = integer(value, "uid", true);
        for (const auto& permission : value.at("permissions")) {
            if (!permission.is_string()) throw std::runtime_error("Invalid account permission type");
            const auto text = permission.get<std::string>();
            require_identifier(text, "account permission");
            if (!result.permissions.insert(text).second) throw std::runtime_error("Duplicate account permission");
        }
        if (!result.permissions.count("SPOT")) throw std::runtime_error("Account has no Spot permission");
        if (value.at("balances").size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
            throw std::runtime_error("Account balance count is out of range");
        for (const auto& balance : value.at("balances")) {
            const auto asset = identifier(balance, "asset");
            const auto free = decimal(balance, "free"), locked = decimal(balance, "locked");
            if (!std::isfinite(free + locked)) throw std::runtime_error("Account balance total overflowed");
            if (!result.balances.emplace(asset, std::make_pair(free, locked)).second)
                throw std::runtime_error("Duplicate account asset balance");
        }
        return result;
    }
    static const std::pair<double, double>& required_balance(const Account& account, const std::string& asset) {
        const auto found = account.balances.find(asset);
        if (found == account.balances.end()) throw std::runtime_error("Account snapshot omitted required asset " + asset);
        return found->second;
    }
    static OpenOrders parse_open_orders(const nlohmann::json& value) {
        if (!value.is_array()) throw std::runtime_error("Open-order snapshot must be an array");
        OpenOrders result;
        std::set<std::pair<std::string, std::string>> clients;
        for (const auto& item : value) {
            const auto symbol = identifier(item, "symbol"), client = identifier(item, "clientOrderId");
            const auto id = integer(item, "orderId", true);
            const auto side = identifier(item, "side");
            if (side != "BUY" && side != "SELL") throw std::runtime_error("Invalid open-order side");
            if (terminal(state(identifier(item, "status")))) throw std::runtime_error("Open-order snapshot contains a terminal order");
            const auto requested = decimal(item, "origQty", true), executed = decimal(item, "executedQty");
            const auto quote = decimal(item, "cummulativeQuoteQty");
            if (executed > requested || ((executed == 0.0) != (quote == 0.0)))
                throw std::runtime_error("Invalid open-order quantities");
            if (!result.emplace(std::make_pair(symbol, id), item).second || !clients.emplace(symbol, client).second)
                throw std::runtime_error("Duplicate open-order identity");
        }
        return result;
    }
    static int scoped_order_count(const OpenOrders& orders, const std::string& symbol) {
        const auto count = std::count_if(orders.begin(), orders.end(), [&](const auto& item) { return item.first.first == symbol; });
        if (count > std::numeric_limits<int>::max()) throw std::runtime_error("Open-order count is out of range");
        return static_cast<int>(count);
    }
    static void validate_durable(const order::Snapshot& value, const std::string& base, const std::string& quote) {
        require_identifier(value.client_order_id, "durable client order identity");
        if (!value.locally_owned || value.exchange_order_id < 0 ||
            (value.side != order::Side::Buy && value.side != order::Side::Sell) ||
            !std::isfinite(value.requested_quantity) || value.requested_quantity <= 0.0 ||
            !std::isfinite(value.executed_quantity) || value.executed_quantity < 0.0 ||
            !std::isfinite(value.cumulative_quote_quantity) || value.cumulative_quote_quantity < 0.0 ||
            !std::isfinite(value.average_fill_price) || value.average_fill_price < 0.0 ||
            value.executed_quantity > value.requested_quantity ||
            ((value.executed_quantity == 0.0) != (value.cumulative_quote_quantity == 0.0)) ||
            (value.executed_quantity > 0.0 && !near(value.average_fill_price, value.cumulative_quote_quantity / value.executed_quantity)) ||
            (!value.base_asset.empty() && value.base_asset != base) ||
            (!value.quote_asset.empty() && value.quote_asset != quote))
            throw std::runtime_error("Durable order is unowned or has inconsistent execution evidence");
        for (const auto& alias : value.client_order_aliases) require_identifier(alias, "durable order alias");
    }
    static order::Snapshot verify_query(const nlohmann::json& value, const order::Snapshot& known) {
        auto result = known;
        const auto symbol = identifier(value, "symbol"), client = identifier(value, "clientOrderId");
        const auto original = value.contains("origClientOrderId") ? identifier(value, "origClientOrderId") : std::string{};
        const bool alias = std::find(known.client_order_aliases.begin(), known.client_order_aliases.end(), client) != known.client_order_aliases.end();
        const auto exchange_id = integer(value, "orderId", true);
        const auto side = identifier(value, "side");
        const auto requested = decimal(value, "origQty", true);
        if (symbol != known.symbol ||
            (client != known.client_order_id && !alias && original != known.client_order_id) ||
            (!original.empty() && original != known.client_order_id) ||
            (known.exchange_order_id > 0 && known.exchange_order_id != exchange_id) ||
            side != (known.side == order::Side::Buy ? "BUY" : "SELL") || !near(requested, known.requested_quantity))
            throw std::runtime_error("Order query contradicts durable ownership");
        result.exchange_order_id = exchange_id;
        result.state = state(identifier(value, "status"));
        result.executed_quantity = decimal(value, "executedQty");
        result.cumulative_quote_quantity = decimal(value, "cummulativeQuoteQty");
        if (result.executed_quantity > requested ||
            ((result.executed_quantity == 0.0) != (result.cumulative_quote_quantity == 0.0)) ||
            result.executed_quantity < known.executed_quantity ||
            result.cumulative_quote_quantity < known.cumulative_quote_quantity ||
            (known.state == order::State::Filled && result.state != order::State::Filled) ||
            (result.state == order::State::Filled && !near(result.executed_quantity, requested)) ||
            (result.state == order::State::Rejected && result.executed_quantity != 0.0))
            throw std::runtime_error("Order query contains contradictory cumulative execution");
        result.average_fill_price = result.executed_quantity == 0.0 ? 0.0 : result.cumulative_quote_quantity / result.executed_quantity;
        if (!std::isfinite(result.average_fill_price)) throw std::runtime_error("Recovered entry price overflowed");
        result.last_event_time_ms = integer(value, "updateTime", true);
        result.updated_at = time_point(result.last_event_time_ms);
        if (client != known.client_order_id && !alias) result.client_order_aliases.push_back(client);
        return result;
    }
    static bool same_order(const order::Snapshot& a, const order::Snapshot& b) {
        return a.symbol == b.symbol && a.client_order_id == b.client_order_id &&
            a.exchange_order_id == b.exchange_order_id && a.side == b.side && a.state == b.state &&
            a.executed_quantity == b.executed_quantity && a.cumulative_quote_quantity == b.cumulative_quote_quantity &&
            a.last_event_time_ms == b.last_event_time_ms && a.client_order_aliases == b.client_order_aliases;
    }
    static Trade parse_trade(const nlohmann::json& value, const std::string& symbol) {
        if (!value.is_object() || identifier(value, "symbol") != symbol || !value.at("isBuyer").is_boolean())
            throw std::runtime_error("Invalid account trade identity");
        Trade trade;
        trade.id = integer(value, "id");
        trade.order_id = integer(value, "orderId", true);
        trade.time = integer(value, "time", true);
        (void)time_point(trade.time);
        trade.side = value.at("isBuyer").get<bool>() ? order::Side::Buy : order::Side::Sell;
        (void)decimal(value, "price", true);
        trade.quantity = decimal(value, "qty", true);
        trade.quote = decimal(value, "quoteQty", true);
        trade.commission = decimal(value, "commission");
        trade.commission_asset = identifier(value, "commissionAsset");
        return trade;
    }
    static void check_deadline(std::chrono::steady_clock::time_point deadline) {
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("Incomplete account recovery: monotonic deadline exceeded");
    }
    static std::int64_t now_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }
    void record(std::int64_t started, const ReconciliationReport& report) {
        if (events_) events_->reconciliation(started, now_ms(), report.success, report.open_orders,
                                             report.balances_checked, report.inconsistencies, report.details);
    }
    BinanceSpotExecutionClient& client_;
    operations::OperationalEventRepository* events_;
};

} // namespace sentum::execution
