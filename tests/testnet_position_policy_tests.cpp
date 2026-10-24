#include <sentum/trader/execution/AccountReconciler.hpp>
#include <sentum/trader/execution/TestnetStrategyRuntime.hpp>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>

namespace sentum::execution {
struct TestnetStrategyRuntimeTestAccess {
    static void start_without_stream(TestnetStrategyRuntime& runtime) {
        runtime.running_.store(true);
        runtime.venue_->configure_market(runtime.symbol_, runtime.risk_);
        runtime.venue_->start([&runtime](const order::Snapshot& update) { runtime.on_order_update(update); });
    }
    static void price(TestnetStrategyRuntime& runtime, double value) { runtime.on_price(value); }
    static void recover(TestnetStrategyRuntime& runtime, const ExecutionRecoveryState& value) { runtime.on_recovery(value); }
    static double quantity(const TestnetStrategyRuntime& runtime) { return runtime.confirmed_quantity_; }
    static double stop(const TestnetStrategyRuntime& runtime) { return runtime.stop_loss_price_; }
    static double highest(const TestnetStrategyRuntime& runtime) { return runtime.highest_price_; }
    static std::chrono::system_clock::time_point entry_time(const TestnetStrategyRuntime& runtime) { return runtime.entry_time_; }
};
}

namespace {
using namespace std::chrono_literals;
using nlohmann::json;
using Time = std::chrono::system_clock::time_point;
using sentum::execution::AccountReconciler;
using sentum::execution::ExecutionRecoveryState;
using sentum::execution::TestnetStrategyRuntime;
using Access = sentum::execution::TestnetStrategyRuntimeTestAccess;
using sentum::order::OrderEventRepository;
using sentum::order::Request;
using sentum::order::Side;
using sentum::order::Snapshot;
using sentum::order::State;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
void near(double value, double expected, const std::string& message) {
    require(std::isfinite(value) && std::abs(value - expected) <= 1e-10 * std::max(1.0, std::abs(expected)), message);
}
template<class Action> void rejects(Action action, const std::string& message) {
    try { action(); } catch (const std::exception&) { return; }
    throw std::runtime_error(message);
}
std::string decimal(double value) {
    std::ostringstream output;
    output << std::setprecision(17) << value;
    return output.str();
}
std::int64_t milliseconds(Time time) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count();
}

class Workspace {
public:
    Workspace() : previous_(std::filesystem::current_path()) {
        std::string pattern = (std::filesystem::temp_directory_path() / "sentum-position-policy-XXXXXX").string();
        const auto created = ::mkdtemp(pattern.data());
        if (!created) throw std::runtime_error("Cannot create position policy fixture");
        path_ = created;
        std::filesystem::current_path(path_);
        std::filesystem::create_directory("log");
    }
    ~Workspace() {
        std::error_code ignored;
        std::filesystem::current_path(previous_, ignored);
        std::filesystem::remove_all(path_, ignored);
    }
    std::string journal() const { return (path_ / "execution.sqlite3").string(); }
    void sql(const std::string& statement) const {
        sqlite3* database = nullptr;
        require(sqlite3_open(journal().c_str(), &database) == SQLITE_OK, "Cannot open fixture SQL connection");
        char* error = nullptr;
        const int result = sqlite3_exec(database, statement.c_str(), nullptr, nullptr, &error);
        const std::string message = error ? error : "Fixture SQL failed";
        sqlite3_free(error);
        sqlite3_close(database);
        require(result == SQLITE_OK, message);
    }
    void fail_policy_writes() const {
        sql("CREATE TRIGGER reject_policy_write BEFORE INSERT ON execution_position_policies "
            "BEGIN SELECT RAISE(ABORT, 'policy checkpoint disk failure'); END;");
    }
private:
    std::filesystem::path previous_, path_;
};

class BuyStrategy final : public IStrategy {
public:
    StrategySignal on_price(double price, Time now) override {
        return {TradeAction::BUY, "fixture", "entry", price, now, 1.0};
    }
    void reset() override {}
};

// The recovery path obtains independent account, terminal-order and per-trade
// responses through the production reconciler. Price extremes are absent from
// those responses: only the runtime's durable checkpoint can retain them.
class Exchange final : public BinanceSpotExecutionClient {
public:
    Exchange() : BinanceSpotExecutionClient("position-policy-fixture", "fixture-secret") {}
    Snapshot fill(const Request& request, double price, Time time, bool partial = false) {
        Snapshot result;
        result.symbol = request.symbol;
        result.client_order_id = request.client_order_id;
        result.exchange_order_id = ++order_id_;
        result.side = request.side;
        result.state = partial ? State::PartiallyFilled : State::Filled;
        result.requested_quantity = request.quantity;
        result.executed_quantity = partial ? request.quantity / 2.0 : request.quantity;
        result.cumulative_quote_quantity = result.executed_quantity * price;
        result.average_fill_price = price;
        result.updated_at = time;
        result.last_event_time_ms = milliseconds(time);
        result.locally_owned = true;
        result.base_asset = "BTC";
        result.quote_asset = "USDT";
        const auto trade_id = ++trade_id_;
        result.commissions[trade_id] = {"USDT", 0.0};
        result.execution_trade_quantities[trade_id] = result.executed_quantity;
        orders_[request.client_order_id] = {
            {"symbol", request.symbol}, {"clientOrderId", request.client_order_id},
            {"orderId", result.exchange_order_id}, {"side", request.side == Side::Buy ? "BUY" : "SELL"},
            {"origQty", decimal(request.quantity)}, {"executedQty", decimal(result.executed_quantity)},
            {"cummulativeQuoteQty", decimal(result.cumulative_quote_quantity)},
            {"status", partial ? "PARTIALLY_FILLED" : "FILLED"}, {"updateTime", result.last_event_time_ms}};
        trades_.push_back({{"symbol", request.symbol}, {"id", trade_id}, {"orderId", result.exchange_order_id},
            {"price", decimal(price)}, {"qty", decimal(result.executed_quantity)},
            {"quoteQty", decimal(result.cumulative_quote_quantity)}, {"time", result.last_event_time_ms},
            {"isBuyer", request.side == Side::Buy}, {"commission", "0"}, {"commissionAsset", "USDT"}});
        base_ += request.side == Side::Buy ? result.executed_quantity : -result.executed_quantity;
        return result;
    }
    void confirm_cancel(const Snapshot& order) {
        auto& summary = orders_.at(order.client_order_id);
        summary["status"] = "CANCELED";
        summary["updateTime"] = order.last_event_time_ms;
    }
    json account() const override {
        return {{"canTrade", true}, {"accountType", "SPOT"}, {"permissions", {"SPOT"}},
            {"updateTime", 1600000000000LL}, {"uid", 12345}, {"balances", json::array({
                {{"asset", "BTC"}, {"free", decimal(base_)}, {"locked", "0"}},
                {{"asset", "USDT"}, {"free", "10000"}, {"locked", "0"}}
            })}};
    }
    json open_orders() const override { return json::array(); }
    json query_order(const std::string& symbol, const std::string& client) const override {
        require(symbol == "BTCUSDT", "Unexpected recovery order market");
        return orders_.at(client);
    }
    json account_trades(const std::string& symbol, std::int64_t from_id, int limit) const override {
        require(symbol == "BTCUSDT", "Unexpected recovery trade market");
        json page = json::array();
        for (const auto& trade : trades_) {
            if (trade.at("id").get<std::int64_t>() >= from_id) page.push_back(trade);
            if (page.size() == static_cast<std::size_t>(limit)) break;
        }
        return page;
    }
private:
    std::map<std::string, json> orders_;
    json trades_ = json::array();
    std::int64_t order_id_ = 100, trade_id_ = 1000;
    double base_ = 0.0;
};

class Venue final : public sentum::execution::IExecutionVenue {
public:
    Venue(std::string path, std::shared_ptr<Exchange> exchange, std::shared_ptr<IClock> clock)
        : journal_(path, OrderEventRepository::account_fingerprint("position-policy-fixture")),
          exchange_(std::move(exchange)), clock_(std::move(clock)) {}
    void configure_market(const std::string& symbol, const RiskConfig&) override {
        require(symbol == "BTCUSDT", "Unexpected configured market");
        if (fail_configuration) throw std::runtime_error("Fixture market configuration failed");
    }
    void start(UpdateHandler handler) override { handler_ = std::move(handler); ready_ = true; }
    void stop() noexcept override {
        ready_ = false;
        ++stop_calls;
        if (!finished_) {
            try { journal_.finish_session(); finished_ = true; } catch (...) {}
        }
    }
    Snapshot submit(const Request& request) override {
        require(ready(), "Fixture order submitted while blocked");
        requests.push_back(request);
        Snapshot intent;
        intent.symbol = request.symbol;
        intent.client_order_id = request.client_order_id;
        intent.side = request.side;
        intent.state = State::Pending;
        intent.requested_quantity = request.quantity;
        intent.updated_at = clock_->now();
        intent.locally_owned = true;
        intent.base_asset = "BTC";
        intent.quote_asset = "USDT";
        journal_.save_order(intent, "local-intent");
        const auto confirmed = exchange_->fill(request, market_price, clock_->now(), keep_buy_open && request.side == Side::Buy);
        journal_.save_order(confirmed, "exchange");
        if (confirmed.state == State::PartiallyFilled) open_buy_ = confirmed;
        handler_(confirmed);
        return confirmed;
    }
    void confirm_buy_cancel() {
        require(open_buy_.has_value(), "No partially filled fixture buy to cancel");
        auto terminal = *open_buy_;
        terminal.state = State::Cancelled;
        terminal.updated_at = clock_->now();
        terminal.last_event_time_ms = milliseconds(terminal.updated_at);
        exchange_->confirm_cancel(terminal);
        journal_.save_order(terminal, "exchange");
        open_buy_.reset();
        handler_(terminal);
    }
    Snapshot cancel(const std::string&) override { throw std::logic_error("No live fixture order"); }
    void kill() noexcept override {
        killed_ = true;
        ready_ = false;
        try { journal_.set_kill_switch(true, "Position policy persistence failed"); } catch (...) {}
    }
    bool ready() const noexcept override { return ready_ && !killed_; }
    bool killed() const noexcept override { return killed_; }
    const char* name() const noexcept override { return "position-policy-fixture"; }
    std::optional<json> load_position_policy(const std::string& symbol) const override {
        return journal_.load_position_policy(symbol);
    }
    void save_position_policy(const std::string& symbol, const json& policy) override {
        journal_.save_position_policy(symbol, policy);
    }
    ExecutionRecoveryState recover_account() {
        const auto recovered = AccountReconciler(*exchange_).recover("BTCUSDT", "BTC", "USDT", journal_.load_latest());
        require(recovered.report.success, "Independent position recovery failed: " + recovered.report.details);
        return {"BTCUSDT", recovered.orders, recovered.replay, recovered.position, recovered.entry_time, recovered.last_exit};
    }
    bool durable_kill() const { return journal_.load_kill_switch(); }
    double market_price = 100.0;
    bool fail_configuration = false;
    bool keep_buy_open = false;
    int stop_calls = 0;
    std::vector<Request> requests;
private:
    OrderEventRepository journal_;
    std::shared_ptr<Exchange> exchange_;
    std::shared_ptr<IClock> clock_;
    UpdateHandler handler_;
    std::optional<Snapshot> open_buy_;
    bool ready_ = false, killed_ = false, finished_ = false;
};

RiskConfig risk() {
    RiskConfig result;
    result.max_total_capital = 100.0;
    result.risk_per_trade = 0.02;
    result.cooldown_seconds = 0;
    result.take_profit_percent = 1.0;
    result.trailing_sl_enabled = true;
    result.trailing_sl_percent = 0.05;
    result.buy_fee_percent = result.sell_fee_percent = result.slippage_percent = result.spread_percent = 0.0;
    return result;
}

void durable_restart_preserves_market_observed_stop() {
    Workspace workspace;
    const auto clock = std::make_shared<ReplayClock>(Time{1600000000s});
    const auto exchange = std::make_shared<Exchange>();
    Time opened;
    {
        auto venue = std::make_unique<Venue>(workspace.journal(), exchange, clock);
        auto* store = venue.get();
        TestnetStrategyRuntime runtime("BTCUSDT", risk(), std::make_unique<BuyStrategy>(), std::move(venue), clock);
        Access::start_without_stream(runtime);
        Access::price(runtime, 100.0);
        opened = Access::entry_time(runtime);
        require(!store->killed(), "Initial policy save failed");
        clock->advance_to(clock->now() + 1s);
        Access::price(runtime, 110.0);
        near(Access::stop(runtime), 104.5, "Runtime did not tighten the trailing stop");
        const auto saved = store->load_position_policy("BTCUSDT");
        require(saved.has_value(), "Observed market policy was not durable");
        near(saved->at("highest_price"), 110.0, "Durable checkpoint lost the observed high");
        Access::recover(runtime, store->recover_account());
        near(Access::stop(runtime), 104.5, "Same-process recovery loosened the stop");
    }
    {
        auto venue = std::make_unique<Venue>(workspace.journal(), exchange, clock);
        auto* store = venue.get();
        TestnetStrategyRuntime runtime("BTCUSDT", risk(), std::make_unique<BuyStrategy>(), std::move(venue), clock);
        Access::start_without_stream(runtime);
        const auto recovered = store->recover_account();
        require(recovered.position.has_value(), "Fresh account proof lost the owned entry");
        Access::recover(runtime, recovered);
        near(Access::highest(runtime), 110.0, "Restart inferred high-water from entry instead of durable policy");
        near(Access::stop(runtime), 104.5, "Restart loosened the durable stop");
        require(Access::entry_time(runtime) == opened, "Restart extended the maximum holding window");
        clock->advance_to(clock->now() + 1s);
        store->market_price = 104.0;
        Access::price(runtime, 104.0);
        require(store->requests.size() == 1 && store->requests.front().side == Side::Sell,
                "Recovered stop did not exit on the first 104 tick");
        near(Access::quantity(runtime), 0.0, "Recovered stop did not close confirmed inventory");
        require(!store->killed(), "Successful recovered stop unexpectedly blocked the venue");
    }
}

void failed_tightening_retains_prior_policy_and_blocks() {
    Workspace workspace;
    const auto clock = std::make_shared<ReplayClock>(Time{1600000000s});
    const auto exchange = std::make_shared<Exchange>();
    auto venue = std::make_unique<Venue>(workspace.journal(), exchange, clock);
    auto* store = venue.get();
    TestnetStrategyRuntime runtime("BTCUSDT", risk(), std::make_unique<BuyStrategy>(), std::move(venue), clock);
    Access::start_without_stream(runtime);
    Access::price(runtime, 100.0);
    const auto before = store->load_position_policy("BTCUSDT");
    workspace.fail_policy_writes();
    Access::price(runtime, 110.0);
    require(store->killed() && !store->ready() && store->durable_kill(), "Failed policy write did not block execution durably");
    near(Access::highest(runtime), 100.0, "Failed write published an undurable high-water mark");
    near(Access::stop(runtime), 98.0, "Failed write published an undurable trailing stop");
    require(store->load_position_policy("BTCUSDT") == before, "Failed checkpoint transaction damaged prior protection");
    Access::price(runtime, 97.0);
    require(store->requests.size() == 1, "A policy write failure permitted another order");
}

void partial_buy_retains_protection_until_terminal() {
    Workspace workspace;
    const auto clock = std::make_shared<ReplayClock>(Time{1600000000s});
    const auto exchange = std::make_shared<Exchange>();
    auto venue = std::make_unique<Venue>(workspace.journal(), exchange, clock);
    auto* store = venue.get();
    store->keep_buy_open = true;
    TestnetStrategyRuntime runtime("BTCUSDT", risk(), std::make_unique<BuyStrategy>(), std::move(venue), clock);
    Access::start_without_stream(runtime);
    Access::price(runtime, 100.0);
    near(Access::quantity(runtime), 0.5, "Partially filled buy did not establish confirmed inventory");
    clock->advance_to(clock->now() + 1s);
    Access::price(runtime, 110.0);
    near(Access::stop(runtime), 104.5, "Active partial buy discarded the favorable price tick");
    near(store->load_position_policy("BTCUSDT")->at("stop_loss_price"), 104.5,
         "Partial-order trailing protection was not persisted");
    Access::price(runtime, 104.0);
    require(store->requests.size() == 1, "Stop crossing submitted while a buy reservation remained active");
    store->confirm_buy_cancel();
    clock->advance_to(clock->now() + 1s);
    store->market_price = 104.0;
    Access::price(runtime, 104.0);
    require(store->requests.size() == 2 && store->requests.back().side == Side::Sell,
            "Terminal partial buy lost its established trailing exit");
    near(store->requests.back().quantity, 0.5, "Trailing exit sold requested instead of confirmed partial inventory");
    near(Access::quantity(runtime), 0.0, "Trailing exit retained confirmed partial inventory");
    require(!store->killed(), "Partial buy protection caused an unexpected failure");
}

void initial_policy_and_recovery_write_failures_are_closed() {
    Workspace workspace;
    const auto clock = std::make_shared<ReplayClock>(Time{1600000000s});
    const auto exchange = std::make_shared<Exchange>();
    {
        auto venue = std::make_unique<Venue>(workspace.journal(), exchange, clock);
        auto* store = venue.get();
        TestnetStrategyRuntime runtime("BTCUSDT", risk(), std::make_unique<BuyStrategy>(), std::move(venue), clock);
        Access::start_without_stream(runtime);
        workspace.fail_policy_writes();
        Access::price(runtime, 100.0);
        require(store->killed() && Access::quantity(runtime) > 0.0, "Initial policy failure concealed the confirmed fill");
        require(!store->load_position_policy("BTCUSDT"), "Rejected initial policy write left a checkpoint");
        const auto recovered = store->recover_account();
        rejects([&] { Access::recover(runtime, recovered); }, "Missing durable trailing checkpoint authorized recovery");
        near(Access::stop(runtime), 0.0, "Missing checkpoint invented trailing protection");
    }
    workspace.sql("DROP TRIGGER reject_policy_write;");
}

void recovery_write_failure_keeps_inventory_unpublished() {
    Workspace workspace;
    const auto clock = std::make_shared<ReplayClock>(Time{1600000000s});
    const auto exchange = std::make_shared<Exchange>();
    {
        auto venue = std::make_unique<Venue>(workspace.journal(), exchange, clock);
        TestnetStrategyRuntime runtime("BTCUSDT", risk(), std::make_unique<BuyStrategy>(), std::move(venue), clock);
        Access::start_without_stream(runtime);
        Access::price(runtime, 100.0);
        Access::price(runtime, 110.0);
    }
    auto venue = std::make_unique<Venue>(workspace.journal(), exchange, clock);
    auto* store = venue.get();
    TestnetStrategyRuntime runtime("BTCUSDT", risk(), std::make_unique<BuyStrategy>(), std::move(venue), clock);
    Access::start_without_stream(runtime);
    const auto recovered = store->recover_account();
    workspace.fail_policy_writes();
    rejects([&] { Access::recover(runtime, recovered); }, "Failed recovery checkpoint commit was ignored");
    near(Access::quantity(runtime), 0.0, "Failed recovery save published partial inventory");
    near(Access::stop(runtime), 0.0, "Failed recovery save published partial protection");
    near(store->load_position_policy("BTCUSDT")->at("stop_loss_price"), 104.5,
         "Failed recovery commit damaged the prior durable stop");
}

void configuration_failure_rolls_back_runtime_start() {
    Workspace workspace;
    const auto clock = std::make_shared<ReplayClock>(Time{1600000000s});
    auto venue = std::make_unique<Venue>(workspace.journal(), std::make_shared<Exchange>(), clock);
    auto* store = venue.get();
    store->fail_configuration = true;
    TestnetStrategyRuntime runtime("BTCUSDT", risk(), std::make_unique<BuyStrategy>(), std::move(venue), clock);
    rejects([&] { runtime.start(); }, "Invalid market configuration started the runtime");
    require(!runtime.running() && store->stop_calls == 1, "Early setup exception bypassed start cleanup");
}

} // namespace

int main() {
    try {
        durable_restart_preserves_market_observed_stop();
        failed_tightening_retains_prior_policy_and_blocks();
        partial_buy_retains_protection_until_terminal();
        initial_policy_and_recovery_write_failures_are_closed();
        recovery_write_failure_keeps_inventory_unpublished();
        configuration_failure_rolls_back_runtime_start();
        std::cout << "testnet position policy tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
