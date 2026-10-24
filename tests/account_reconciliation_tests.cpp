#include <sentum/trader/execution/AccountReconciler.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using nlohmann::json;
using sentum::execution::AccountReconciler;
using sentum::execution::AccountRecovery;
using sentum::execution::AccountRecoveryLimits;
using sentum::order::Side;
using sentum::order::Snapshot;
using sentum::order::State;

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
void near(double actual, double expected, const std::string& message) {
    require(std::abs(actual - expected) < 1e-10 * std::max(1.0, std::abs(expected)), message);
}
json account(const std::string& base = "0") {
    return {{"canTrade", true}, {"accountType", "SPOT"}, {"permissions", {"SPOT", "TRD_GRP_004"}},
        {"updateTime", 2000}, {"uid", 12345}, {"balances", json::array({
            {{"asset", "BTC"}, {"free", base}, {"locked", "0"}},
            {{"asset", "USDT"}, {"free", "10000"}, {"locked", "0"}},
            {{"asset", "BNB"}, {"free", "100"}, {"locked", "0"}}
        })}};
}
json summary(const std::string& client = "owned-buy", std::int64_t id = 101,
             const std::string& side = "BUY", const std::string& requested = "2",
             const std::string& executed = "2", const std::string& quote = "215",
             const std::string& status = "FILLED", std::int64_t time = 2000) {
    return {{"symbol", "BTCUSDT"}, {"clientOrderId", client}, {"orderId", id}, {"side", side},
        {"origQty", requested}, {"executedQty", executed}, {"cummulativeQuoteQty", quote},
        {"status", status}, {"updateTime", time}};
}
json trade(std::int64_t id, std::int64_t order_id, const std::string& quantity,
           const std::string& quote, std::int64_t time, bool buyer = true,
           const std::string& fee = "0", const std::string& asset = "USDT") {
    return {{"symbol", "BTCUSDT"}, {"id", id}, {"orderId", order_id}, {"price", "100"},
        {"qty", quantity}, {"quoteQty", quote}, {"time", time}, {"isBuyer", buyer},
        {"commission", fee}, {"commissionAsset", asset}};
}
Snapshot owned(std::string client = "owned-buy", std::int64_t id = 101, Side side = Side::Buy,
               double requested = 2.0) {
    Snapshot value;
    value.symbol = "BTCUSDT";
    value.client_order_id = std::move(client);
    value.exchange_order_id = id;
    value.side = side;
    value.requested_quantity = requested;
    value.state = State::Unknown;
    value.locally_owned = true;
    value.base_asset = "BTC";
    value.quote_asset = "USDT";
    value.updated_at = std::chrono::system_clock::time_point(std::chrono::milliseconds(500));
    return value;
}

class Client final : public BinanceSpotExecutionClient {
public:
    Client() : BinanceSpotExecutionClient("fixture-key", "fixture-secret") {}
    json account() const override {
        ++account_calls;
        if (account_hook) return account_hook(account_calls);
        return account_response;
    }
    json open_orders() const override {
        ++open_calls;
        if (open_hook) return open_hook(open_calls);
        return open_response;
    }
    json query_order(const std::string& symbol, const std::string& client) const override {
        require(symbol == "BTCUSDT", "Unexpected query symbol");
        const auto count = ++query_calls[client];
        if (query_hook) return query_hook(client, count);
        const auto found = orders.find(client);
        if (found == orders.end()) throw BinanceRequestError(BinanceRequestError::Kind::NotFound, "Fixture order missing");
        return found->second;
    }
    json account_trades(const std::string& symbol, std::int64_t from_id, int limit) const override {
        require(symbol == "BTCUSDT", "Unexpected trade symbol");
        cursors.push_back(from_id);
        if (trade_hook) return trade_hook(from_id, limit, cursors.size());
        json page = json::array();
        for (const auto& item : trades) {
            if (item.at("id").get<std::int64_t>() >= from_id) page.push_back(item);
            if (page.size() == static_cast<std::size_t>(limit)) break;
        }
        return page;
    }
    json account_response = ::account();
    json open_response = json::array();
    json trades = json::array();
    std::map<std::string, json> orders;
    mutable int account_calls = 0, open_calls = 0;
    mutable std::map<std::string, int> query_calls;
    mutable std::vector<std::int64_t> cursors;
    std::function<json(int)> account_hook, open_hook;
    std::function<json(const std::string&, int)> query_hook;
    std::function<json(std::int64_t, int, std::size_t)> trade_hook;
};

AccountRecovery recover(Client& client, const std::vector<Snapshot>& orders = {}, AccountRecoveryLimits limits = {}) {
    return AccountReconciler(client).recover("BTCUSDT", "BTC", "USDT", orders, limits);
}
void failed(const AccountRecovery& result, const std::string& message) {
    require(!result.report.success && result.report.inconsistencies > 0 && !result.report.details.empty(), message);
    require(result.orders.empty() && result.replay.empty() && !result.position, "Failure leaked partial authoritative restoration");
}
void filled_fixture(Client& client) {
    client.orders["owned-buy"] = summary();
    client.trades = json::array({trade(11, 101, "0.5", "50", 1000, true, "0.001", "BTC"),
                               trade(12, 101, "1.5", "165", 2000, true, "0.43", "USDT")});
    client.account_response = account("1.999");
}

void malformed_account_and_order_evidence_never_means_flat() {
    Client client;
    const auto original = client.account_response;
    for (const auto& malformed : {json::object(), json::array(), json(nullptr)}) {
        client.account_response = malformed;
        require(!AccountReconciler(client).run("BTCUSDT", 0.0, "BTC").success, "Malformed account falsely proved flat");
    }
    for (const char* required : {"canTrade", "accountType", "permissions", "updateTime", "balances"}) {
        client.account_response = original;
        client.account_response.erase(required);
        failed(recover(client), std::string("Missing account field accepted: ") + required);
    }
    client.account_response = original;
    client.account_response["balances"].erase(client.account_response["balances"].begin());
    failed(recover(client), "Missing zero base row accepted");
    client.account_response = original;
    client.account_response["balances"].erase(client.account_response["balances"].begin() + 1);
    failed(recover(client), "Missing quote row accepted");
    client.account_response = original;
    client.account_response["balances"].push_back(original["balances"][0]);
    failed(recover(client), "Duplicate base rows accepted");
    for (const json bad : {json("nan"), json("inf"), json("-1"), json("1garbage"), json("1e309"), json(" 0"), json(0), json(nullptr)}) {
        client.account_response = original;
        client.account_response["balances"][0]["free"] = bad;
        failed(recover(client), "Malformed free balance accepted");
    }
    client.account_response = original;
    client.account_response["canTrade"] = false;
    failed(recover(client), "Disabled account accepted");
    client.account_response = original;
    client.account_response["accountType"] = "MARGIN";
    failed(recover(client), "Wrong account type accepted");
    client.account_response = original;
    client.account_response["permissions"] = {"SPOT", "SPOT"};
    failed(recover(client), "Duplicate permission accepted");
    client.account_response = original;
    client.account_response["updateTime"] = std::numeric_limits<std::uint64_t>::max();
    failed(recover(client), "Overflowing account timestamp accepted");
    client.account_response = original;
    client.open_response = json::object();
    require(!AccountReconciler(client).run("BTCUSDT", 0.0, "BTC").success, "Non-array open orders falsely proved flat");
    client.open_response = json::array();
    for (double quantity : {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(), -1.0})
        require(!AccountReconciler(client).run("BTCUSDT", quantity, "BTC").success, "Invalid expected inventory accepted");
}

void symbol_scope_and_unexplained_inventory_are_explicit() {
    Client client;
    auto other = summary("external-eth", 200, "BUY", "1", "0", "0", "NEW");
    other["symbol"] = "ETHUSDT";
    auto other2 = other;
    other2["clientOrderId"] = "external-eth-2";
    other2["orderId"] = 201;
    client.open_response = json::array({other, other2});
    const auto report = AccountReconciler(client).run("BTCUSDT", 0.0, "BTC");
    require(report.success && report.open_orders == 0, "Unrelated symbols changed scoped open-order count");
    require(recover(client).report.success, "Stable unrelated account orders blocked a flat scoped reconstruction");
    client.open_response.push_back(summary("external-btc", 300, "BUY", "1", "0", "0", "NEW"));
    failed(recover(client), "Unresolved target-symbol open order authorized recovery");
    client.open_response = json::array();
    client.account_response = account("0.00000000000001");
    failed(recover(client), "Small unexplained inventory was rounded to flat");
    client.account_response = account();
    client.trades = json::array({trade(1, 777, "1", "100", 1000)});
    failed(recover(client), "Unowned trade was attributed to the strategy");
}

void restart_restores_partial_fills_all_fee_assets_and_original_ownership() {
    Client client;
    filled_fixture(client);
    auto durable = owned();
    durable.state = State::PartiallyFilled;
    durable.executed_quantity = 0.5;
    durable.cumulative_quote_quantity = 50.0;
    durable.average_fill_price = 100.0;
    durable.requires_reconciliation = true;
    durable.execution_trade_quantities.emplace(11, 0.5);
    durable.commissions.emplace(11, sentum::order::ExecutionCommission{"BTC", 0.001});
    AccountRecoveryLimits limits;
    limits.page_size = 1;
    const auto result = recover(client, {durable}, limits);
    require(result.report.success, result.report.details);
    require(result.orders.size() == 1 && result.replay.size() == 2 && result.position, "Completed BUY restart lost inventory");
    near(result.position->quantity, 1.999, "Base fee not deducted");
    near(result.position->average_entry_price, 215.43 / 1.999, "Quote fee missing from acquisition cost");
    require(result.orders[0].client_order_id == durable.client_order_id && !result.orders[0].requires_reconciliation,
            "Verified recovery changed ownership or retained incomplete-proof flag");
    require(result.orders[0].commissions.size() == 2 && result.orders[0].execution_trade_quantities.size() == 2,
            "Complete per-trade fee proof missing");
    require(result.entry_time == std::chrono::system_clock::time_point(std::chrono::milliseconds(1000)), "First confirmed entry time lost");
    require(result.account_permissions.count("TRD_GRP_004") && result.account_uid == 12345, "Account authority metadata lost");
    require(client.cursors == std::vector<std::int64_t>({0, 12, 13, 13}), "Trade history did not paginate from the beginning and verify its tail");
    require(result.finished_ms >= result.started_ms && result.started_ms > 0, "Recovery evidence timestamps absent");
    sentum::order::ConfirmedPositionLedger restored;
    for (const auto& snapshot : result.replay) restored.on_order_update(snapshot);
    require(!restored.on_order_update(result.orders[0]).applied, "Repeated terminal summary duplicated restored execution");
    near(restored.get("BTCUSDT")->quantity, 1.999, "Replayed quantity changed on duplicate");

    client.trades[1]["commissionAsset"] = "BNB";
    client.trades[1]["commission"] = "0.01";
    const auto third_asset = recover(client, {durable});
    require(third_asset.report.success, third_asset.report.details);
    near(third_asset.position->average_entry_price, 215.0 / 1.999, "Third-asset fee invented a quote conversion");
    require(third_asset.orders[0].commissions.at(12).asset == "BNB", "Third-asset fee lost its asset");
}

void cancelled_partial_sales_and_zero_fee_proof_survive_restart() {
    Client client;
    client.orders["owned-buy"] = summary("owned-buy", 101, "BUY", "2", "2", "200");
    client.orders["owned-sell"] = summary("owned-sell", 102, "SELL", "2", "0.5", "60", "CANCELED", 3000);
    client.trades = json::array({trade(1, 101, "2", "200", 1000), trade(2, 102, "0.5", "60", 3000, false, "0.06")});
    client.account_response = account("1.5");
    const auto result = recover(client, {owned(), owned("owned-sell", 102, Side::Sell)});
    require(result.report.success, result.report.details);
    near(result.position->quantity, 1.5, "Cancelled partial SELL discarded remaining inventory");
    near(result.position->average_entry_price, 100.0, "Partial SELL changed remaining acquisition basis");
    require(result.orders[0].commissions.count(1) && result.orders[0].commissions.at(1).amount == 0.0,
            "Explicit zero fee proof was discarded");
    client.orders["owned-sell"] = summary("owned-sell", 102, "SELL", "2", "2", "240", "FILLED", 3000);
    client.trades[1]["qty"] = "2";
    client.trades[1]["quoteQty"] = "240";
    client.account_response = account();
    const auto closed = recover(client, {owned(), owned("owned-sell", 102, Side::Sell)});
    require(closed.report.success && !closed.position, closed.report.details);
    require(closed.entry_time == std::chrono::system_clock::time_point{} &&
            closed.last_exit == std::chrono::system_clock::time_point(std::chrono::milliseconds(3000)), "Completed exit chronology lost");
}

void missing_changed_or_malformed_trade_evidence_blocks_recovery() {
    Client client;
    filled_fixture(client);
    auto durable = owned();
    client.trades.erase(client.trades.begin());
    failed(recover(client, {durable}), "Incomplete history passed using order summary alone");
    filled_fixture(client);
    durable.commissions.emplace(11, sentum::order::ExecutionCommission{"BTC", 0.002});
    failed(recover(client, {durable}), "Changed persisted commission accepted");
    durable.commissions.clear();
    durable.execution_trade_quantities.emplace(99, 0.5);
    failed(recover(client, {durable}), "Missing persisted execution identity accepted");
    durable.execution_trade_quantities.clear();
    for (const char* field : {"commission", "commissionAsset", "qty", "quoteQty", "isBuyer", "symbol", "time", "orderId", "id"}) {
        filled_fixture(client);
        client.trades[0].erase(field);
        failed(recover(client, {durable}), std::string("Missing trade field accepted: ") + field);
    }
    filled_fixture(client);
    client.trades[0]["isBuyer"] = false;
    failed(recover(client, {durable}), "Trade side contradiction accepted");
    filled_fixture(client);
    client.trades[0]["qty"] = "nan";
    failed(recover(client, {durable}), "NaN trade accepted");
    filled_fixture(client);
    client.trades[0]["id"] = std::numeric_limits<std::uint64_t>::max();
    failed(recover(client, {durable}), "Overflowing trade identity accepted");
    filled_fixture(client);
    client.trade_hook = [&](std::int64_t, int, std::size_t) { return json::array({client.trades[0]}); };
    AccountRecoveryLimits limits;
    limits.page_size = 1;
    failed(recover(client, {durable}, limits), "Repeated trade page advanced recovery");
    client.trade_hook = {};
    limits.max_pages = 1;
    failed(recover(client, {durable}, limits), "Page limit was reported as complete history");
    limits.max_pages = 64;
    limits.max_replay_entries = 2;
    failed(recover(client, {durable}, limits), "Replay evidence pressure exceeded its retained-entry bound");
}

void concurrent_changes_and_deadlines_do_not_publish_restoration() {
    Client client;
    filled_fixture(client);
    client.account_hook = [&](int call) {
        auto value = client.account_response;
        if (call > 1) value["updateTime"] = 2001;
        return value;
    };
    failed(recover(client, {owned()}), "Changed account snapshot accepted");
    client.account_hook = {};
    client.query_calls.clear();
    client.query_hook = [&](const std::string&, int call) {
        auto value = summary();
        if (call > 1) value["updateTime"] = 2001;
        return value;
    };
    failed(recover(client, {owned()}), "Changed order snapshot accepted");
    client.query_hook = {};
    client.cursors.clear();
    client.trade_hook = [&](std::int64_t, int, std::size_t call) {
        return call == 1 ? client.trades : json::array({trade(13, 101, "0.1", "10", 3000)});
    };
    failed(recover(client, {owned()}), "New trade at recovery tail accepted");
    client.trade_hook = {};
    client.account_hook = [&](int) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return client.account_response;
    };
    AccountRecoveryLimits limits;
    limits.timeout = std::chrono::milliseconds(1);
    failed(recover(client, {owned()}, limits), "Deadline expiry published restoration");
}

void ambiguous_missing_orders_and_ownership_conflicts_stay_blocked() {
    Client client;
    failed(recover(client, {owned("uncertain", 0)}), "NotFound uncertain placement became flat");
    auto rejected = owned("rejected", 0);
    rejected.state = State::Rejected;
    const auto empty = recover(client, {rejected});
    require(empty.report.success && empty.orders.size() == 1, empty.report.details);
    require(client.query_calls.count("rejected") == 0, "Pre-dispatch rejection queried an exchange order");
    filled_fixture(client);
    auto durable = owned();
    durable.locally_owned = false;
    failed(recover(client, {durable}), "Unowned journal order gained ownership");
    durable.locally_owned = true;
    client.orders["owned-buy"]["clientOrderId"] = "foreign";
    failed(recover(client, {durable}), "Foreign query client was accepted");
    filled_fixture(client);
    client.orders["owned-buy"]["orderId"] = 999;
    failed(recover(client, {durable}), "Changed exchange identity accepted");
    filled_fixture(client);
    failed(recover(client, {durable, durable}), "Duplicate durable identity accepted");
    client.orders["owned-buy"]["clientOrderId"] = "cancel-alias";
    client.orders["owned-buy"]["origClientOrderId"] = "owned-buy";
    const auto alias = recover(client, {durable});
    require(alias.report.success && alias.orders[0].client_order_id == "owned-buy" &&
            alias.orders[0].client_order_aliases == std::vector<std::string>{"cancel-alias"}, alias.report.details);
}

} // namespace

int main() {
    try {
        malformed_account_and_order_evidence_never_means_flat();
        symbol_scope_and_unexplained_inventory_are_explicit();
        restart_restores_partial_fills_all_fee_assets_and_original_ownership();
        cancelled_partial_sales_and_zero_fee_proof_survive_restart();
        missing_changed_or_malformed_trade_evidence_blocks_recovery();
        concurrent_changes_and_deadlines_do_not_publish_restoration();
        ambiguous_missing_orders_and_ownership_conflicts_stay_blocked();
        std::cout << "Account reconciliation tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
