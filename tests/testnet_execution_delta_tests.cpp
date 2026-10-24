#include <sentum/trader/execution/TestnetStrategyRuntime.hpp>
#include <sentum/trader/order/ConfirmedPositionLedger.hpp>

#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>

namespace sentum::execution {
struct TestnetStrategyRuntimeTestAccess {
    static void start_without_stream(TestnetStrategyRuntime& runtime) {
        runtime.running_.store(true);
        runtime.venue_->start([&runtime](const order::Snapshot& update) { runtime.on_order_update(update); });
    }
    static void price(TestnetStrategyRuntime& runtime, double value) { runtime.on_price(value); }
    static double quantity(TestnetStrategyRuntime& runtime) {
        std::lock_guard<std::mutex> lock(runtime.mutex_);
        return runtime.confirmed_quantity_;
    }
    static double entry(TestnetStrategyRuntime& runtime) {
        std::lock_guard<std::mutex> lock(runtime.mutex_);
        return runtime.entry_price_;
    }
    static std::string active(TestnetStrategyRuntime& runtime) {
        std::lock_guard<std::mutex> lock(runtime.mutex_);
        return runtime.active_order_;
    }
};
}

namespace {
using sentum::order::ConfirmedPositionLedger;
using sentum::order::Request;
using sentum::order::Snapshot;
using sentum::order::State;
using sentum::order::Side;
using sentum::execution::TestnetStrategyRuntime;
using Access = sentum::execution::TestnetStrategyRuntimeTestAccess;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void close(double actual, double expected, const char* message) {
    if (!std::isfinite(actual) || std::abs(actual - expected) > 1e-10 * std::max(1.0, std::abs(expected)))
        throw std::runtime_error(std::string(message) + ": actual=" + std::to_string(actual) + " expected=" + std::to_string(expected));
}
template<typename Callback>
void rejects(Callback callback, const char* message) {
    bool rejected = false;
    try { callback(); } catch (const std::exception&) { rejected = true; }
    require(rejected, message);
}

Snapshot snapshot(std::int64_t id, Side side, double quantity, double quote, State state = State::PartiallyFilled) {
    Snapshot value;
    value.symbol = "BTCUSDT";
    value.client_order_id = "delta-" + std::to_string(id);
    value.exchange_order_id = id;
    value.side = side;
    value.state = state;
    value.requested_quantity = 5.0;
    value.executed_quantity = quantity;
    value.cumulative_quote_quantity = quote;
    value.average_fill_price = quantity > 0.0 ? quote / quantity : 0.0;
    value.updated_at = std::chrono::system_clock::now();
    value.locally_owned = true;
    value.base_asset = "BTC";
    value.quote_asset = "USDT";
    return value;
}

void cumulative_execution_is_idempotent() {
    ConfirmedPositionLedger ledger;
    auto partial = snapshot(1, Side::Buy, 0.4, 40.0);
    auto delta = ledger.on_order_update(partial);
    require(delta.applied, "partial fill was not applied");
    close(delta.executed_quantity, 0.4, "wrong first quantity delta");
    close(ledger.get("BTCUSDT")->quantity, 0.4, "partial fill did not create owned exposure");
    require(!ledger.on_order_update(partial).applied, "duplicate partial fill was applied twice");
    auto older = snapshot(1, Side::Buy, 0.2, 20.0);
    require(!ledger.on_order_update(older).applied, "older partial fill regressed watermark");
    auto complete = snapshot(1, Side::Buy, 1.0, 106.0, State::Filled);
    delta = ledger.on_order_update(complete);
    close(delta.executed_quantity, 0.6, "complete fill did not use cumulative delta");
    close(delta.quote_quantity, 66.0, "complete fill quote delta is wrong");
    close(ledger.get("BTCUSDT")->quantity, 1.0, "complete fill duplicated earlier execution");
    close(ledger.get("BTCUSDT")->average_entry_price, 106.0, "weighted entry price is wrong");
    require(!ledger.on_order_update(complete).applied, "duplicate complete fill was applied twice");
    older.state = State::Cancelled;
    require(!ledger.on_order_update(older).applied, "stale cancellation erased confirmed fill");
    auto foreign = snapshot(99, Side::Buy, 12.0, 1200.0, State::Filled);
    foreign.locally_owned = false;
    require(!ledger.on_order_update(foreign).applied, "external account order mutated owned inventory");
    close(ledger.get("BTCUSDT")->quantity, 1.0, "external order changed position");
}

void partial_cancellations_and_sells_preserve_inventory() {
    ConfirmedPositionLedger ledger;
    auto cancelled_buy = snapshot(1, Side::Buy, 0.4, 40.0, State::Cancelled);
    ledger.on_order_update(cancelled_buy);
    close(ledger.get("BTCUSDT")->quantity, 0.4, "cancelled partial buy vanished");
    auto partial_sell = snapshot(2, Side::Sell, 0.1, 11.0);
    ledger.on_order_update(partial_sell);
    close(ledger.get("BTCUSDT")->quantity, 0.3, "partial sell removed complete position");
    close(ledger.get("BTCUSDT")->average_entry_price, 100.0, "partial sale changed remaining entry cost");
    partial_sell.state = State::Cancelled;
    require(!ledger.on_order_update(partial_sell).applied, "cancelled partial sell was counted again");
    auto final_sell = snapshot(3, Side::Sell, 0.3, 33.0, State::Filled);
    ledger.on_order_update(final_sell);
    require(!ledger.get("BTCUSDT"), "complete cumulative sales left inventory");
    require(!ledger.on_order_update(final_sell).applied, "duplicate final sell was counted again");
    require(!ledger.on_order_update(cancelled_buy).applied, "restart replay resurrected a completed position");
}

void commission_assets_change_only_the_correct_balances() {
    ConfirmedPositionLedger ledger;
    auto buy = snapshot(1, Side::Buy, 1.0, 100.0);
    buy.commissions = {{101, {"BTC", 0.001}}};
    auto delta = ledger.on_order_update(buy);
    close(ledger.get("BTCUSDT")->quantity, 0.999, "base commission did not reduce purchased inventory");
    close(ledger.get("BTCUSDT")->average_entry_price, 100.0 / 0.999, "base fee acquisition price is wrong");
    close(delta.base_quantity_change, 0.999, "base cash delta is wrong");
    close(delta.quote_balance_change, -100.0, "buy quote cash delta is wrong");

    buy.executed_quantity = 2.0;
    buy.cumulative_quote_quantity = 220.0;
    buy.average_fill_price = 110.0;
    buy.state = State::Filled;
    buy.commissions.emplace(102, sentum::order::ExecutionCommission{"USDT", 0.2});
    buy.commissions.emplace(103, sentum::order::ExecutionCommission{"BNB", 0.003});
    delta = ledger.on_order_update(buy);
    close(ledger.get("BTCUSDT")->quantity, 1.999, "quote/third asset commission reduced base inventory");
    close(ledger.get("BTCUSDT")->average_entry_price, 220.2 / 1.999, "quote commission did not increase acquisition cost");
    close(delta.quote_balance_change, -120.2, "incremental quote commission cash delta is wrong");
    close(delta.commissions.at("BNB"), 0.003, "third asset fee was lost");
    require(!ledger.on_order_update(buy).applied, "duplicate commission traded twice");

    auto sell = snapshot(2, Side::Sell, 0.5, 60.0, State::Cancelled);
    sell.commissions = {{201, {"BTC", 0.001}}, {202, {"USDT", 0.12}}, {203, {"BNB", 0.002}}};
    delta = ledger.on_order_update(sell);
    close(ledger.get("BTCUSDT")->quantity, 1.498, "sell base fee did not reduce remaining inventory");
    close(ledger.get("BTCUSDT")->average_entry_price, 220.2 / 1.999, "sell commissions changed remaining entry price");
    close(delta.quote_balance_change, 59.88, "sell quote fee did not reduce proceeds");
    const auto fees = ledger.commission_totals("BTCUSDT");
    close(fees.at("BTC"), 0.002, "base fee total is wrong");
    close(fees.at("USDT"), 0.32, "quote fee total is wrong");
    close(fees.at("BNB"), 0.005, "third asset fees were converted or lost");
}

void older_trade_commissions_complete_newer_execution_totals() {
    ConfirmedPositionLedger ledger;
    auto complete = snapshot(1, Side::Buy, 1.0, 106.0, State::Filled);
    complete.commissions = {{11, {"USDT", 0.066}}};
    ledger.on_order_update(complete);
    auto earlier = snapshot(1, Side::Buy, 0.4, 40.0);
    earlier.commissions = {{10, {"USDT", 0.04}}};
    const auto delta = ledger.on_order_update(earlier);
    require(delta.applied, "older trade's unseen commission was discarded");
    close(delta.executed_quantity, 0.0, "older commission report duplicated execution");
    close(ledger.get("BTCUSDT")->quantity, 1.0, "older commission report regressed inventory");
    close(ledger.get("BTCUSDT")->average_entry_price, 106.106, "reordered commission did not update acquisition cost");
    require(!ledger.on_order_update(earlier).applied, "reordered commission was applied twice");
}

void delayed_commissions_reprice_the_original_position() {
    ConfirmedPositionLedger ledger;
    auto buy = snapshot(1, Side::Buy, 1.0, 100.0, State::Filled);
    ledger.on_order_update(buy);
    ledger.on_order_update(snapshot(2, Side::Sell, 0.4, 44.0, State::Filled));
    buy.commissions.emplace(10, sentum::order::ExecutionCommission{"USDT", 1.0});
    ledger.on_order_update(buy);
    close(ledger.get("BTCUSDT")->quantity, 0.6, "delayed quote fee changed remaining inventory");
    close(ledger.get("BTCUSDT")->average_entry_price, 101.0, "delayed quote fee was allocated wholly to remainder");
    ledger.on_order_update(snapshot(3, Side::Sell, 0.6, 66.0, State::Filled));
    ledger.on_order_update(snapshot(4, Side::Buy, 1.0, 200.0, State::Filled));
    buy.commissions.emplace(11, sentum::order::ExecutionCommission{"USDT", 1.0});
    ledger.on_order_update(buy);
    close(ledger.get("BTCUSDT")->quantity, 1.0, "late closed-order commission changed a later position");
    close(ledger.get("BTCUSDT")->average_entry_price, 200.0, "late closed-order fee contaminated a later entry");
    close(ledger.commission_totals("BTCUSDT").at("USDT"), 2.0, "delayed fee was not retained");
    require(!ledger.on_order_update(buy).applied, "delayed fee duplicate was reapplied");
}

void invalid_execution_does_not_consume_watermarks() {
    ConfirmedPositionLedger ledger;
    auto buy = snapshot(1, Side::Buy, 1.0, 100.0);
    auto malformed = buy;
    malformed.commissions = {{1, {"BTC", 0.1}}};
    malformed.base_asset.clear();
    rejects([&] { ledger.on_order_update(malformed); }, "commission without asset metadata was accepted");
    require(!ledger.get("BTCUSDT"), "failed commission update changed position");
    ledger.on_order_update(buy);
    malformed = buy;
    malformed.client_order_id = "other-client";
    rejects([&] { ledger.on_order_update(malformed); }, "exchange order ID accepted different client ownership");
    malformed = buy;
    malformed.exchange_order_id = 100;
    rejects([&] { ledger.on_order_update(malformed); }, "owned client identity accepted a second exchange order");
    malformed = buy;
    malformed.side = Side::Sell;
    rejects([&] { ledger.on_order_update(malformed); }, "exchange order ID accepted different side");
    malformed = buy;
    malformed.executed_quantity = std::numeric_limits<double>::quiet_NaN();
    rejects([&] { ledger.on_order_update(malformed); }, "nonfinite execution was accepted");
    malformed = buy;
    malformed.executed_quantity = 2.0;
    malformed.cumulative_quote_quantity = 99.0;
    rejects([&] { ledger.on_order_update(malformed); }, "contradictory cumulative execution was accepted");
    rejects([&] { ledger.on_order_update(snapshot(2, Side::Sell, 2.0, 200.0)); }, "oversell erased owned inventory");
    close(ledger.get("BTCUSDT")->quantity, 1.0, "rejected updates consumed execution state");
    buy.commissions = {{1, {"BTC", 0.001}}};
    ledger.on_order_update(buy);
    malformed = buy;
    malformed.commissions.at(1).amount = 0.002;
    rejects([&] { ledger.on_order_update(malformed); }, "same trade commission changed after application");
    close(ledger.get("BTCUSDT")->quantity, 0.999, "rejected commission consumed watermark");
    ledger.on_order_update(snapshot(2, Side::Sell, 0.999, 100.0, State::Filled));
    require(!ledger.get("BTCUSDT"), "valid sale after rejection failed");
}

void explicit_zero_fee_is_immutable_without_invented_asset_totals() {
    ConfirmedPositionLedger ledger;
    auto buy = snapshot(1, Side::Buy, 1.0, 100.0, State::Filled);
    buy.commissions = {{100, {"", 0.0}}};
    ledger.on_order_update(buy);
    require(ledger.commission_totals("BTCUSDT").empty(), "zero fee invented an asset balance");
    require(!ledger.on_order_update(buy).applied, "explicit zero-fee duplicate was applied twice");
    auto changed = buy;
    changed.commissions.at(100) = {"USDT", 0.1};
    rejects([&] { ledger.on_order_update(changed); }, "explicit zero fee changed to nonzero without reconciliation");
    close(ledger.get("BTCUSDT")->quantity, 1.0, "changed zero fee modified inventory");
    close(ledger.get("BTCUSDT")->average_entry_price, 100.0, "changed zero fee modified entry cost");
}

void late_base_commission_failure_preserves_watermarks() {
    ConfirmedPositionLedger ledger;
    auto buy = snapshot(1, Side::Buy, 1.0, 100.0, State::Filled);
    buy.commissions = {{100, {"", 0.0}}};
    ledger.on_order_update(buy);
    ledger.on_order_update(snapshot(2, Side::Sell, 1.0, 110.0, State::Filled));
    buy.commissions = {{101, {"BTC", 0.001}}};
    rejects([&] { ledger.on_order_update(buy); }, "late base fee silently allowed negative inventory");
    require(!ledger.get("BTCUSDT"), "failed historical correction changed completed position");
    require(ledger.commission_totals("BTCUSDT").empty(), "failed historical correction consumed fee watermark");
    buy.commissions = {{101, {"USDT", 0.1}}};
    require(ledger.on_order_update(buy).applied, "valid correction after rejected fee was not applied");
    close(ledger.commission_totals("BTCUSDT").at("USDT"), 0.1, "rejected fee contaminated valid correction");
    require(!ledger.get("BTCUSDT"), "quote fee resurrected completed inventory");
}

void replay_keeps_identical_inventory_and_fees() {
    auto partial = snapshot(1, Side::Buy, 0.4, 40.0);
    partial.commissions = {{10, {"USDT", 0.04}}};
    auto complete = snapshot(1, Side::Buy, 1.0, 106.0, State::Filled);
    complete.commissions = {{10, {"USDT", 0.04}}, {11, {"USDT", 0.066}}};
    auto sold = snapshot(2, Side::Sell, 0.25, 30.0, State::Cancelled);
    sold.commissions = {{12, {"BNB", 0.001}}};
    std::vector<Snapshot> history{partial, partial, complete, partial, complete, sold, sold};
    ConfirmedPositionLedger live, replayed;
    for (const auto& value : history) live.on_order_update(value);
    for (const auto& value : history) replayed.on_order_update(value);
    for (const auto& value : history) require(!replayed.on_order_update(value).applied, "repeated history replay applied a new execution");
    close(replayed.get("BTCUSDT")->quantity, live.get("BTCUSDT")->quantity, "replay changed inventory");
    close(replayed.get("BTCUSDT")->average_entry_price, live.get("BTCUSDT")->average_entry_price, "replay changed entry cost");
    require(replayed.commission_totals("BTCUSDT") == live.commission_totals("BTCUSDT"), "replay changed fee totals");
}

class Workspace {
public:
    Workspace() : previous_(std::filesystem::current_path()) {
        std::string pattern = (std::filesystem::temp_directory_path() / "sentum-execution-deltas-XXXXXX").string();
        const auto* directory = ::mkdtemp(pattern.data());
        if (!directory) throw std::runtime_error("Cannot create execution fixture");
        path_ = directory;
        std::filesystem::create_directory(path_ / "log");
        std::filesystem::current_path(path_);
    }
    ~Workspace() {
        std::filesystem::current_path(previous_);
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
private:
    std::filesystem::path previous_, path_;
};

class BuyStrategy final : public IStrategy {
public:
    StrategySignal on_price(double price, std::chrono::system_clock::time_point now) override {
        return {TradeAction::BUY, "fixture", "entry", price, now, 1.0};
    }
    void reset() override { ++resets; }
    int resets = 0;
};

class ManualVenue final : public sentum::execution::IExecutionVenue {
public:
    void start(UpdateHandler callback) override { callback_ = std::move(callback); ready_ = true; }
    void stop() noexcept override { ready_ = false; }
    Snapshot submit(const Request& request) override {
        latest = request;
        ++submissions;
        latest_exchange_id = submissions;
        auto pending = update(State::Pending, 0.0, 0.0);
        pending.exchange_order_id = 0;
        callback_(pending);
        return pending;
    }
    Snapshot cancel(const std::string&) override { return {}; }
    void kill() noexcept override { killed_ = true; ready_ = false; }
    bool ready() const noexcept override { return ready_ && !killed_; }
    bool killed() const noexcept override { return killed_; }
    const char* name() const noexcept override { return "execution-delta-fixture"; }
    Snapshot update(State state, double quantity, double quote) const {
        auto value = snapshot(latest_exchange_id, latest.side, quantity, quote, state);
        value.client_order_id = latest.client_order_id;
        value.requested_quantity = latest.quantity;
        return value;
    }
    void emit(const Snapshot& value) { callback_(value); }
    Request latest;
    int submissions = 0;
    std::int64_t latest_exchange_id = 0;
private:
    UpdateHandler callback_;
    bool ready_ = false, killed_ = false;
};

RiskConfig risk() { RiskConfig config; config.cooldown_seconds = 0; return config; }

void runtime_manages_partial_inventory_and_ownership() {
    Workspace workspace;
    auto venue = std::make_unique<ManualVenue>();
    auto* exchange = venue.get();
    auto strategy = std::make_unique<BuyStrategy>();
    auto* signals = strategy.get();
    TestnetStrategyRuntime runtime("BTCUSDT", risk(), std::move(strategy), std::move(venue));
    Access::start_without_stream(runtime);
    Access::price(runtime, 100.0);
    const double bought = exchange->latest.quantity * 0.4;
    auto partial = exchange->update(State::PartiallyFilled, bought, bought * 100.0);
    exchange->emit(partial);
    close(Access::quantity(runtime), bought, "runtime ignored partial buy");
    require(!Access::active(runtime).empty(), "partial fill cleared active request");
    exchange->emit(partial);
    close(Access::quantity(runtime), bought, "runtime duplicated partial inventory");
    auto unrelated = partial;
    unrelated.client_order_id = "manual-order";
    unrelated.exchange_order_id = 777;
    unrelated.executed_quantity = 12.0;
    unrelated.cumulative_quote_quantity = 1200.0;
    exchange->emit(unrelated);
    unrelated = partial;
    unrelated.symbol = "ETHUSDT";
    exchange->emit(unrelated);
    close(Access::quantity(runtime), bought, "unrelated account order mutated runtime inventory");
    partial.state = State::Cancelled;
    exchange->emit(partial);
    require(Access::active(runtime).empty(), "terminal partial buy retained request reservation");
    close(Access::quantity(runtime), bought, "cancelled buy lost confirmed partial inventory");
    Access::price(runtime, 110.0);
    require(exchange->latest.side == Side::Sell, "retained partial buy was not managed by exit policy");
    close(exchange->latest.quantity, bought, "exit did not use confirmed partial quantity");
    const auto sale_id = Access::active(runtime);
    exchange->emit(partial);
    require(Access::active(runtime) == sale_id && !exchange->killed(), "old order duplicate cleared or killed active sale");
    auto sale = exchange->update(State::PartiallyFilled, bought * 0.25, bought * 0.25 * 110.0);
    exchange->emit(sale);
    close(Access::quantity(runtime), bought * 0.75, "runtime partial sale cleared all inventory");
    close(Access::entry(runtime), 100.0, "partial exit changed entry cost");
    sale.state = State::Cancelled;
    exchange->emit(sale);
    require(signals->resets == 0, "strategy reset before remaining inventory was sold");
    Access::price(runtime, 110.0);
    close(exchange->latest.quantity, bought * 0.75, "replacement exit ignored remaining inventory");
    auto final_sale = exchange->update(State::Filled, exchange->latest.quantity, exchange->latest.quantity * 110.0);
    exchange->emit(final_sale);
    exchange->emit(final_sale);
    close(Access::quantity(runtime), 0.0, "final exit did not close position");
    require(signals->resets == 1, "duplicate final exit reset strategy twice");
    runtime.stop();
}

void runtime_retains_incomplete_execution_and_blocks_new_orders() {
    Workspace workspace;
    auto venue = std::make_unique<ManualVenue>();
    auto* exchange = venue.get();
    TestnetStrategyRuntime runtime("BTCUSDT", risk(), std::make_unique<BuyStrategy>(), std::move(venue));
    Access::start_without_stream(runtime);
    Access::price(runtime, 100.0);
    const double gross = exchange->latest.quantity;
    auto fill = exchange->update(State::Filled, gross, gross * 100.0);
    fill.requires_reconciliation = true;
    exchange->emit(fill);
    close(Access::quantity(runtime), gross, "incomplete history discarded confirmed gross exposure");
    require(exchange->killed(), "missing execution history did not block new orders");
    Access::price(runtime, 110.0);
    require(exchange->submissions == 1, "runtime sold gross inventory with unknown commissions");
    runtime.stop();
}

void runtime_fee_inventory_and_identity_failure() {
    Workspace workspace;
    auto venue = std::make_unique<ManualVenue>();
    auto* exchange = venue.get();
    TestnetStrategyRuntime runtime("BTCUSDT", risk(), std::make_unique<BuyStrategy>(), std::move(venue));
    Access::start_without_stream(runtime);
    Access::price(runtime, 100.0);
    const double gross = exchange->latest.quantity;
    auto fill = exchange->update(State::Filled, gross, gross * 100.0);
    fill.commissions = {{1, {"BTC", gross * 0.001}}};
    exchange->emit(fill);
    close(Access::quantity(runtime), gross * 0.999, "runtime ignored base commission");
    Access::price(runtime, 110.0);
    close(exchange->latest.quantity, gross * 0.999, "runtime tried to sell unavailable base commission");
    auto ack = exchange->update(State::Acknowledged, 0.0, 0.0);
    exchange->emit(ack);
    auto forged = ack;
    forged.exchange_order_id += 100;
    forged.state = State::Filled;
    forged.executed_quantity = gross * 0.999;
    forged.cumulative_quote_quantity = forged.executed_quantity * 110.0;
    forged.average_fill_price = 110.0;
    exchange->emit(forged);
    require(exchange->killed(), "conflicting exchange identity did not fail closed");
    close(Access::quantity(runtime), gross * 0.999, "conflicting identity cleared confirmed inventory");
    runtime.stop();
}
}

int main() {
    try {
        cumulative_execution_is_idempotent();
        partial_cancellations_and_sells_preserve_inventory();
        commission_assets_change_only_the_correct_balances();
        older_trade_commissions_complete_newer_execution_totals();
        delayed_commissions_reprice_the_original_position();
        invalid_execution_does_not_consume_watermarks();
        explicit_zero_fee_is_immutable_without_invented_asset_totals();
        late_base_commission_failure_preserves_watermarks();
        replay_keeps_identical_inventory_and_fees();
        runtime_manages_partial_inventory_and_ownership();
        runtime_fee_inventory_and_identity_failure();
        runtime_retains_incomplete_execution_and_blocks_new_orders();
        std::cout << "testnet_execution_delta_tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "testnet_execution_delta_tests: " << error.what() << '\n';
        return 1;
    }
}
