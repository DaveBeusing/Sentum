#include <sentum/api/BinanceEnvironment.hpp>
#include <sentum/api/BinanceWebsocketClient.hpp>
#include <sentum/core/RuntimeControl.hpp>
#include <sentum/time/Clock.hpp>
#include <sentum/trader/TradeEngine.hpp>
#include <sentum/trader/execution/TestnetStrategyRuntime.hpp>
#include <sentum/trader/risk/PositionExitPolicy.hpp>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
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
    static double quantity(const TestnetStrategyRuntime& runtime) { return runtime.confirmed_quantity_; }
    static double entry(const TestnetStrategyRuntime& runtime) { return runtime.entry_price_; }
    static double stop(const TestnetStrategyRuntime& runtime) { return runtime.stop_loss_price_; }
    static std::string reason(const TestnetStrategyRuntime& runtime) { return runtime.exit_reason_; }
    static void recover(TestnetStrategyRuntime& runtime, const ExecutionRecoveryState& recovered) {
        runtime.on_recovery(recovered);
    }
};
}
struct MarketStreamTestAccess {
    static void deliver(BinanceWebsocketClient& client, double price) {
        client.running.store(true);
        client.handle_message(nlohmann::json{{"p", std::to_string(price)}}.dump(), false);
    }
};

namespace {
using namespace std::chrono_literals;
using Time = std::chrono::system_clock::time_point;
using sentum::api::BinanceEnvironment;
using sentum::order::Request;
using sentum::order::Snapshot;
using sentum::order::Side;
using sentum::order::State;
using sentum::execution::TestnetStrategyRuntime;
using Access = sentum::execution::TestnetStrategyRuntimeTestAccess;
void require(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }
void near(double actual, double expected, const std::string& message) {
    require(std::isfinite(actual) && std::abs(actual - expected) <= 1e-10 * std::max(1.0, std::abs(expected)), message);
}
template<class Action> void rejects(Action action, const std::string& message) {
    try { action(); } catch (const std::exception&) { return; }
    throw std::runtime_error(message);
}
class Workspace {
public:
    Workspace() : previous_(std::filesystem::current_path()) {
        std::string pattern = (std::filesystem::temp_directory_path() / "sentum-market-policy-XXXXXX").string();
        const auto created = ::mkdtemp(pattern.data());
        if (!created) throw std::runtime_error("Cannot create market policy fixture");
        path_ = created;
        std::filesystem::current_path(path_);
        std::filesystem::create_directory("log");
    }
    ~Workspace() {
        std::error_code ignored;
        std::filesystem::current_path(previous_, ignored);
        std::filesystem::remove_all(path_, ignored);
    }
private:
    std::filesystem::path previous_, path_;
};
class BuyStrategy final : public IStrategy {
public:
    StrategySignal on_price(double price, Time now) override { return {TradeAction::BUY, "fixture", "entry", price, now, 1.0}; }
    void reset() override {}
};
class ConfirmingVenue final : public sentum::execution::IExecutionVenue {
public:
    struct DurableState {
        std::optional<nlohmann::json> checkpoint;
        std::int64_t next_order_id = 0;
    };
    explicit ConfirmingVenue(std::shared_ptr<IClock> clock,
                             BinanceEnvironment environment = BinanceEnvironment::SpotTestnet,
                             std::shared_ptr<DurableState> durable = std::make_shared<DurableState>())
        : clock_(std::move(clock)), environment_(environment), durable_(std::move(durable)) {}
    void start(UpdateHandler handler) override { handler_ = std::move(handler); ready_ = true; }
    void stop() noexcept override { ready_ = false; }
    Snapshot submit(const Request& request) override {
        require(ready(), "fixture submit while blocked");
        requests.push_back(request);
        Snapshot result;
        result.symbol = request.symbol;
        result.client_order_id = request.client_order_id;
        result.exchange_order_id = ++durable_->next_order_id;
        result.side = request.side;
        result.state = State::Filled;
        result.requested_quantity = request.quantity;
        result.executed_quantity = request.quantity;
        result.average_fill_price = market_price;
        result.cumulative_quote_quantity = request.quantity * market_price;
        result.locally_owned = true;
        result.base_asset = "BTC";
        result.quote_asset = "USDT";
        result.updated_at = clock_->now();
        result.last_event_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(result.updated_at.time_since_epoch()).count();
        const auto trade_id = result.exchange_order_id * 10;
        result.commissions[trade_id] = {"USDT", 0.0};
        result.execution_trade_quantities[trade_id] = result.executed_quantity;
        if (request.side == Side::Buy && partial_buy_with_base_fee) {
            result.state = State::PartiallyFilled;
            result.executed_quantity = 0.5;
            result.cumulative_quote_quantity = 0.5 * market_price;
            result.commissions[trade_id] = {"BTC", 0.005};
            result.execution_trade_quantities[trade_id] = 0.5;
            handler_(result);
            result.executed_quantity = 1.0;
            result.cumulative_quote_quantity = market_price;
            result.commissions[trade_id + 1] = {"BTC", 0.005};
            result.execution_trade_quantities[trade_id + 1] = 0.5;
            handler_(result);
            result.state = State::Cancelled;
        } else if (request.side == Side::Sell && partial_first_sale) {
            partial_first_sale = false;
            result.executed_quantity = request.quantity / 2.0;
            result.cumulative_quote_quantity = result.executed_quantity * market_price;
            result.execution_trade_quantities[trade_id] = result.executed_quantity;
            result.state = State::PartiallyFilled;
            handler_(result);
            result.state = State::Cancelled;
        }
        reports.push_back(result);
        handler_(result);
        return result;
    }
    Snapshot cancel(const std::string&) override { throw std::logic_error("fixture has no active remote order"); }
    void kill() noexcept override { killed_ = true; ready_ = false; }
    bool ready() const noexcept override { return ready_ && !killed_; }
    bool killed() const noexcept override { return killed_; }
    const char* name() const noexcept override { return "testnet-fixture"; }
    BinanceEnvironment market_environment() const noexcept override { return environment_; }
    std::optional<nlohmann::json> load_position_policy(const std::string& symbol) const override {
        if (durable_->checkpoint) require(durable_->checkpoint->at("symbol") == symbol, "checkpoint market mismatch");
        return durable_->checkpoint;
    }
    void save_position_policy(const std::string& symbol, const nlohmann::json& value) override {
        require(value.at("symbol") == symbol, "persisted policy market mismatch");
        durable_->checkpoint = value;
    }
    void duplicate_last_report() { handler_(reports.back()); }
    double market_price = 100.0;
    bool partial_buy_with_base_fee = false;
    bool partial_first_sale = false;
    std::vector<Request> requests;
    std::vector<Snapshot> reports;
private:
    std::shared_ptr<IClock> clock_;
    BinanceEnvironment environment_;
    std::shared_ptr<DurableState> durable_;
    UpdateHandler handler_;
    bool ready_ = false, killed_ = false;
};
RiskConfig policy(bool trailing, double distance = 0.05) {
    RiskConfig risk;
    risk.cooldown_seconds = 0;
    risk.take_profit_percent = 1.0;
    risk.trailing_sl_enabled = trailing;
    risk.trailing_sl_percent = distance;
    risk.buy_fee_percent = risk.sell_fee_percent = risk.spread_percent = risk.slippage_percent = 0.0;
    return risk;
}

void same_policy_sequence(const RiskConfig& risk, const std::vector<double>& prices, const std::string& reason) {
    Workspace workspace;
    auto clock = std::make_shared<ReplayClock>(Time{1600000000s});
    TradeEngine paper("BTCUSDT", risk, clock, std::make_unique<BuyStrategy>());
    auto venue = std::make_unique<ConfirmingVenue>(clock);
    auto* exchange = venue.get();
    TestnetStrategyRuntime testnet("BTCUSDT", risk, std::make_unique<BuyStrategy>(), std::move(venue), clock);
    Access::start_without_stream(testnet);
    for (double price : prices) {
        clock->advance_to(clock->now() + 1s);
        exchange->market_price = price;
        MarketEvent event; event.symbol = "BTCUSDT"; event.timestamp = clock->now(); event.price = price; event.close = price;
        const auto action = paper.process_event(event);
        const auto before = exchange->requests.size();
        Access::price(testnet, price);
        require(!exchange->killed(), "policy sequence latched unexpected Testnet failure");
        const auto position = paper.get_current_position();
        if (action == TradeAction::BUY) {
            require(exchange->requests.size() == before + 1 && exchange->requests.back().side == Side::Buy,
                    "simulated and Testnet entry times differ");
        } else if (action == TradeAction::SELL) {
            require(exchange->requests.size() == before + 1 && exchange->requests.back().side == Side::Sell,
                    "simulated and Testnet exit times differ");
            require(paper.completed_trades().back().exit_time == exchange->reports.back().updated_at,
                    "simulated and Testnet exit timestamps differ");
            require(paper.completed_trades().back().close_reason == Access::reason(testnet),
                    "simulated and Testnet exit reasons differ");
            near(Access::quantity(testnet), 0.0, "Testnet retained a completely sold position");
        } else {
            require(exchange->requests.size() == before, "Testnet made an exit absent from simulated policy");
        }
        if (position.open) {
            near(Access::quantity(testnet), position.quantity, "confirmed policy quantity differs");
            near(Access::entry(testnet), position.entry_price, "confirmed policy entry differs");
            near(Access::stop(testnet), position.stop_loss_price, "trailing stop policies diverged");
            require(Access::stop(testnet) >= position.entry_price * (1.0 - risk.stop_loss_percent),
                    "favorable price weakened initial protection");
        }
    }
    require(paper.completed_trades().size() == 1 && paper.completed_trades().front().close_reason == reason,
            "price sequence failed to exercise expected exit reason");
}

void policy_parity() {
    sentum::runtime::RuntimeControl::global().pause_entries(false);
    (void)sentum::runtime::RuntimeControl::global().consume_manual_close();
    same_policy_sequence(policy(true), {100.0, 110.0, 105.0, 104.5}, "stop_loss");
    same_policy_sequence(policy(false), {100.0, 110.0, 104.5, 98.0}, "stop_loss");
    auto wide = policy(true, 0.10); wide.stop_loss_percent = 0.01;
    same_policy_sequence(wide, {100.0, 105.0, 110.0, 111.0, 100.0, 99.9}, "stop_loss");
    auto target = policy(false); target.take_profit_percent = 0.03;
    same_policy_sequence(target, {100.0, 101.0, 103.0}, "take_profit");
    auto timeout = policy(false); timeout.max_holding_seconds = 3;
    same_policy_sequence(timeout, {100.0, 100.1, 100.1, 100.1}, "maximum_holding_time");
}

void environment_and_price_isolation() {
    Workspace workspace;
    BinanceWebsocketClient testnet_feed("TESTNETONLYUSDT", 2000ms, BinanceEnvironment::SpotTestnet);
    BinanceWebsocketClient production_feed("TESTNETONLYUSDT", 2000ms, BinanceEnvironment::PublicMarketData);
    require(testnet_feed.environment() == BinanceEnvironment::SpotTestnet &&
            testnet_feed.endpoint() == "wss://stream.testnet.binance.vision/ws/testnetonlyusdt@trade",
            "Testnet-only symbol selected another market endpoint");
    require(std::string(sentum::api::market_stream_host(testnet_feed.environment())) == "stream.testnet.binance.vision",
            "Testnet endpoint and verified TLS hostname diverged");
    require(production_feed.endpoint() == "wss://stream.binance.com:9443/ws/testnetonlyusdt@trade",
            "public-data endpoint contract changed");
    auto clock = std::make_shared<ReplayClock>(Time{1600000000s});
    rejects([&] {
        TestnetStrategyRuntime wrong("BTCUSDT", policy(true), std::make_unique<BuyStrategy>(),
            std::make_unique<ConfirmingVenue>(clock, BinanceEnvironment::PublicMarketData), clock);
    }, "Testnet runtime accepted production market provenance");
    auto venue = std::make_unique<ConfirmingVenue>(clock);
    auto* exchange = venue.get();
    TestnetStrategyRuntime runtime("TESTNETONLYUSDT", policy(true), std::make_unique<BuyStrategy>(), std::move(venue), clock);
    Access::start_without_stream(runtime);
    int production_events = 0;
    production_feed.set_on_price([&](double) { ++production_events; });
    testnet_feed.set_on_price([&](double price) { exchange->market_price = price; Access::price(runtime, price); });
    MarketStreamTestAccess::deliver(testnet_feed, 100.0);
    require(exchange->requests.size() == 1 && exchange->requests.front().symbol == "TESTNETONLYUSDT",
            "Testnet-only market failed to enter from its own data");
    MarketStreamTestAccess::deliver(production_feed, 90.0);
    require(production_events == 1 && exchange->requests.size() == 1,
            "divergent production price triggered Testnet risk");
    clock->advance_to(clock->now() + 1s);
    MarketStreamTestAccess::deliver(testnet_feed, 110.0);
    MarketStreamTestAccess::deliver(production_feed, 1.0);
    require(exchange->requests.size() == 1, "production movement changed Testnet trailing decision");
    clock->advance_to(clock->now() + 1s);
    MarketStreamTestAccess::deliver(testnet_feed, 104.0);
    require(exchange->requests.size() == 2 && exchange->requests.back().side == Side::Sell && Access::reason(runtime) == "stop_loss",
            "Testnet price did not trigger its own trailing exit");
}

void partial_inventory_and_commission_policy() {
    Workspace workspace;
    auto clock = std::make_shared<ReplayClock>(Time{1600000000s});
    auto venue = std::make_unique<ConfirmingVenue>(clock);
    auto* exchange = venue.get();
    exchange->partial_buy_with_base_fee = true;
    exchange->partial_first_sale = true;
    TestnetStrategyRuntime runtime("BTCUSDT", policy(true, 0.02), std::make_unique<BuyStrategy>(), std::move(venue), clock);
    Access::start_without_stream(runtime);
    Access::price(runtime, 100.0);
    near(Access::quantity(runtime), 0.99, "partial base-commission inventory was not authoritative");
    near(Access::entry(runtime), 100.0 / 0.99, "entry policy omitted confirmed acquisition fees");
    near(Access::stop(runtime), (100.0 / 0.99) * 0.98, "initial stop used requested instead of confirmed fill cost");
    clock->advance_to(clock->now() + 1s); exchange->market_price = 110.0; Access::price(runtime, 110.0);
    const auto trailing_stop = Access::stop(runtime);
    near(trailing_stop, 107.8, "partial position did not advance shared trailing policy");
    exchange->duplicate_last_report();
    near(Access::stop(runtime), trailing_stop, "duplicate confirmed report reset trailing protection");
    clock->advance_to(clock->now() + 1s); exchange->market_price = 107.0; Access::price(runtime, 107.0);
    near(exchange->requests.back().quantity, 0.99, "exit submitted gross or requested acquisition quantity");
    near(Access::quantity(runtime), 0.495, "partial sell discarded remaining confirmed inventory");
    near(Access::stop(runtime), trailing_stop, "partial sell weakened established trailing stop");
    clock->advance_to(clock->now() + 1s); Access::price(runtime, 107.0);
    near(exchange->requests.back().quantity, 0.495, "remaining exit duplicated already sold inventory");
    near(Access::quantity(runtime), 0.0, "remaining net inventory did not close");
    require(exchange->requests.size() == 3 && !exchange->killed(), "partial-fill exit sequence unexpectedly failed");
}

sentum::execution::ExecutionRecoveryState recovered_position(const std::vector<Snapshot>& evidence) {
    sentum::execution::ExecutionRecoveryState recovered;
    recovered.symbol = "BTCUSDT";
    recovered.orders = evidence;
    recovered.replay = evidence;
    sentum::order::ConfirmedPositionLedger ledger;
    ledger.restore(evidence);
    recovered.position = ledger.get(recovered.symbol);
    recovered.entry_time = evidence.front().updated_at;
    require(recovered.position.has_value(), "fixture recovery has no confirmed position");
    return recovered;
}

void checkpoint_recovery_preserves_protection() {
    for (bool cold_restart : {false, true}) {
        Workspace workspace;
        auto clock = std::make_shared<ReplayClock>(Time{1600000000s});
        auto durable = std::make_shared<ConfirmingVenue::DurableState>();
        auto venue = std::make_unique<ConfirmingVenue>(clock, BinanceEnvironment::SpotTestnet, durable);
        auto* exchange = venue.get();
        auto runtime = std::make_unique<TestnetStrategyRuntime>("BTCUSDT", policy(true),
            std::make_unique<BuyStrategy>(), std::move(venue), clock);
        Access::start_without_stream(*runtime);
        Access::price(*runtime, 100.0);
        clock->advance_to(clock->now() + 1s);
        exchange->market_price = 110.0;
        Access::price(*runtime, 110.0);
        near(Access::stop(*runtime), 104.5, "favorable price failed to establish trailing checkpoint");
        require(durable->checkpoint.has_value(), "confirmed position policy was not persisted");
        near(durable->checkpoint->at("highest_price").get<double>(), 110.0,
             "persisted policy lost its favorable price");
        const auto recovered = recovered_position(exchange->reports);
        if (cold_restart) {
            runtime.reset();
            auto next_venue = std::make_unique<ConfirmingVenue>(clock, BinanceEnvironment::SpotTestnet, durable);
            exchange = next_venue.get();
            runtime = std::make_unique<TestnetStrategyRuntime>("BTCUSDT", policy(true),
                std::make_unique<BuyStrategy>(), std::move(next_venue), clock);
        }
        Access::recover(*runtime, recovered);
        near(Access::stop(*runtime), 104.5, "recovery weakened a verified trailing stop");
        near(Access::quantity(*runtime), recovered.position->quantity, "recovery changed confirmed inventory");
        if (cold_restart) Access::start_without_stream(*runtime);
        const auto before = exchange->requests.size();
        clock->advance_to(clock->now() + 1s);
        exchange->market_price = 104.0;
        Access::price(*runtime, 104.0);
        require(exchange->requests.size() == before + 1 && exchange->requests.back().side == Side::Sell &&
                Access::reason(*runtime) == "stop_loss" && !exchange->killed(),
                "recovered high-water mark did not cause the established trailing exit");
        near(Access::quantity(*runtime), 0.0, "recovered trailing exit did not close confirmed inventory");
    }
}

void invalid_checkpoint_recovery_is_atomic() {
    Workspace workspace;
    auto clock = std::make_shared<ReplayClock>(Time{1600000000s});
    auto durable = std::make_shared<ConfirmingVenue::DurableState>();
    sentum::execution::ExecutionRecoveryState recovered;
    {
        auto venue = std::make_unique<ConfirmingVenue>(clock, BinanceEnvironment::SpotTestnet, durable);
        auto* exchange = venue.get();
        TestnetStrategyRuntime original("BTCUSDT", policy(true), std::make_unique<BuyStrategy>(), std::move(venue), clock);
        Access::start_without_stream(original);
        Access::price(original, 100.0);
        clock->advance_to(clock->now() + 1s);
        exchange->market_price = 110.0;
        Access::price(original, 110.0);
        recovered = recovered_position(exchange->reports);
    }
    const auto valid_checkpoint = durable->checkpoint;
    require(valid_checkpoint.has_value(), "fixture lacks persisted recovery evidence");
    for (int corruption = 0; corruption != 4; ++corruption) {
        durable->checkpoint = valid_checkpoint;
        if (corruption == 0) durable->checkpoint.reset();
        else if (corruption == 1) (*durable->checkpoint)["risk_policy"]["trailing_sl_percent"] = 0.01;
        else if (corruption == 2) (*durable->checkpoint)["entry_exchange_order_id"] = 9999;
        else (*durable->checkpoint)["risk_policy"].erase("stop_loss_percent");
        const auto rejected_evidence = durable->checkpoint;
        auto venue = std::make_unique<ConfirmingVenue>(clock, BinanceEnvironment::SpotTestnet, durable);
        auto* exchange = venue.get();
        TestnetStrategyRuntime runtime("BTCUSDT", policy(true), std::make_unique<BuyStrategy>(), std::move(venue), clock);
        rejects([&] { Access::recover(runtime, recovered); }, "invalid trailing checkpoint was accepted");
        near(Access::quantity(runtime), 0.0, "failed recovery published unprotected inventory");
        near(Access::stop(runtime), 0.0, "failed recovery published unverified protection");
        Access::price(runtime, 104.0);
        require(!runtime.running() && !exchange->ready() && exchange->requests.empty() &&
                durable->checkpoint == rejected_evidence,
                "failed recovery changed evidence or allowed an order before readiness");
    }
}
} // namespace

int main() {
    try {
        policy_parity(); environment_and_price_isolation(); partial_inventory_and_commission_policy();
        checkpoint_recovery_preserves_protection(); invalid_checkpoint_recovery_is_atomic();
        std::cout << "Testnet market and policy tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Testnet market and policy tests failed: " << error.what() << '\n';
        return 1;
    }
}
