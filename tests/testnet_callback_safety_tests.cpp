#include <sentum/trader/execution/TestnetStrategyRuntime.hpp>
#include <sentum/trader/order/OrderManager.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>

namespace sentum::execution {
struct TestnetStrategyRuntimeTestAccess {
    static void start_without_stream(TestnetStrategyRuntime& runtime) {
        runtime.running_.store(true);
        runtime.venue_->start([&runtime](const order::Snapshot& update) { runtime.on_order_update(update); });
    }
    static void price(TestnetStrategyRuntime& runtime, double value) { runtime.on_price(value); }
    static std::string active(TestnetStrategyRuntime& runtime) {
        std::lock_guard<std::mutex> lock(runtime.mutex_);
        return runtime.active_order_;
    }
    static double quantity(TestnetStrategyRuntime& runtime) {
        std::lock_guard<std::mutex> lock(runtime.mutex_);
        return runtime.confirmed_quantity_;
    }
    static bool submitting(TestnetStrategyRuntime& runtime) {
        std::lock_guard<std::mutex> lock(runtime.mutex_);
        return runtime.submission_in_progress_;
    }
};
}

namespace {
using namespace std::chrono_literals;
using sentum::execution::TestnetStrategyRuntime;
using Access = sentum::execution::TestnetStrategyRuntimeTestAccess;
using sentum::order::Request;
using sentum::order::Snapshot;
using sentum::order::State;
using sentum::order::Side;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Future>
void require_ready(Future& future, const char* description) {
    if (future.wait_for(5s) != std::future_status::ready) {
        std::cerr << "testnet_callback_safety timeout: " << description << '\n';
        std::_Exit(2);
    }
}

template<class Callback>
void bounded(Callback&& callback, const char* description) {
    auto future = std::async(std::launch::async, std::forward<Callback>(callback));
    require_ready(future, description);
    future.get();
}

class Workspace {
public:
    Workspace() : previous_(std::filesystem::current_path()) {
        std::string pattern = (std::filesystem::temp_directory_path() / "sentum-testnet-callback-XXXXXX").string();
        const auto* path = ::mkdtemp(pattern.data());
        if (!path) throw std::runtime_error("Cannot create callback fixture");
        path_ = path;
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
        return {TradeAction::BUY, "fixture", "buy", price, now, 1.0};
    }
    void reset() override {}
};

Snapshot snapshot(const Request& request, State state) {
    Snapshot result;
    result.symbol = request.symbol;
    result.client_order_id = request.client_order_id;
    result.side = request.side;
    result.state = state;
    result.requested_quantity = request.quantity;
    result.updated_at = std::chrono::system_clock::now();
    result.locally_owned = true;
    result.base_asset = "BTC";
    result.quote_asset = "USDT";
    if (state != State::Pending && state != State::Rejected)
        result.exchange_order_id = request.side == Side::Buy ? 42 : 43;
    if (state == State::Filled) {
        result.executed_quantity = request.quantity;
        result.average_fill_price = 100.0;
        result.cumulative_quote_quantity = result.executed_quantity * result.average_fill_price;
    }
    return result;
}

class SyncVenue final : public sentum::execution::IExecutionVenue {
public:
    std::optional<nlohmann::json> load_position_policy(const std::string&) const override { return policy_; }
    void save_position_policy(const std::string&, const nlohmann::json& policy) override { policy_ = policy; }
    void start(UpdateHandler handler) override { handler_ = std::move(handler); ready_.store(true); }
    void stop() noexcept override { ready_.store(false); ++stops; }
    Snapshot submit(const Request& request) override {
        ++submissions;
        latest = request;
        handler_(snapshot(request, State::Pending));
        if (throw_on_submit) throw std::runtime_error("fixture submit failure");
        handler_(snapshot(request, result_state));
        if (after_callback) after_callback();
        return snapshot(request, return_stale_ack ? State::Acknowledged : result_state);
    }
    Snapshot cancel(const std::string&) override { return {}; }
    void kill() noexcept override { killed_.store(true); ready_.store(false); ++kills; }
    bool ready() const noexcept override { return ready_.load() && !killed_.load(); }
    bool killed() const noexcept override { return killed_.load(); }
    const char* name() const noexcept override { return "callback-fixture"; }
    void update(const Snapshot& value) { handler_(value); }
    State result_state = State::Acknowledged;
    bool return_stale_ack = false;
    bool throw_on_submit = false;
    std::atomic<int> submissions{0}, kills{0}, stops{0};
    std::function<void()> after_callback;
    Request latest;
private:
    UpdateHandler handler_;
    std::optional<nlohmann::json> policy_;
    std::atomic<bool> ready_{false}, killed_{false};
};

class MockExchange final : public BinanceSpotExecutionClient {
public:
    MockExchange() : BinanceSpotExecutionClient("fixture", "fixture") {}
    nlohmann::json open_orders() const override { return nlohmann::json::array(); }
    nlohmann::json place_market_order(const Request& request) const override {
        ++placements;
        latest = request;
        if (during_placement) during_placement(request);
        if (reject) throw BinanceRequestError(BinanceRequestError::Kind::Rejected, "fixture rejection", -2010);
        return {{"symbol", request.symbol}, {"clientOrderId", request.client_order_id}, {"orderId", 42}};
    }
    nlohmann::json query_order(const std::string&, const std::string&) const override {
        throw std::runtime_error("fixture order outcome is unavailable");
    }
    nlohmann::json cancel_order(const std::string& symbol, const std::string& id) const override {
        ++cancellations;
        return {{"symbol", symbol}, {"origClientOrderId", id}, {"clientOrderId", "cancel-" + id},
                {"orderId", 42}, {"side", latest.side == Side::Buy ? "BUY" : "SELL"},
                {"origQty", latest.quantity}, {"executedQty", "0"},
                {"cummulativeQuoteQty", "0"}, {"status", "CANCELED"}};
    }
    mutable std::atomic<int> placements{0}, cancellations{0};
    mutable Request latest;
    bool reject = false;
    std::function<void(const Request&)> during_placement;
};

class ManagedVenue final : public sentum::execution::IExecutionVenue {
public:
    std::optional<nlohmann::json> load_position_policy(const std::string&) const override { return policy_; }
    void save_position_policy(const std::string&, const nlohmann::json& policy) override { policy_ = policy; }
    ManagedVenue() : manager(exchange) {}
    void start(UpdateHandler handler) override { manager.set_update_handler(std::move(handler)); manager.reconcile_startup(); }
    void stop() noexcept override { ++stops; manager.activate_kill_switch(); }
    Snapshot submit(const Request& request) override { return manager.submit_market(request); }
    Snapshot cancel(const std::string& id) override { return manager.cancel(id); }
    void kill() noexcept override { ++kills; manager.activate_kill_switch(); }
    bool ready() const noexcept override { return manager.reconciled() && !manager.kill_switch_active(); }
    bool killed() const noexcept override { return manager.kill_switch_active(); }
    const char* name() const noexcept override { return "managed-fixture"; }
    MockExchange exchange;
    sentum::order::OrderManager manager;
    std::atomic<int> stops{0}, kills{0};
    std::optional<nlohmann::json> policy_;
};

RiskConfig risk() {
    RiskConfig result;
    result.cooldown_seconds = 0;
    return result;
}

void synchronous_runtime_states() {
    for (const auto state : {State::Acknowledged, State::Rejected, State::Filled}) {
        Workspace workspace;
        auto venue = std::make_unique<SyncVenue>();
        auto* mock = venue.get();
        mock->result_state = state;
        mock->return_stale_ack = state == State::Filled;
        TestnetStrategyRuntime runtime("BTCUSDT", risk(), std::make_unique<BuyStrategy>(), std::move(venue));
        Access::start_without_stream(runtime);
        bounded([&] { Access::price(runtime, 100.0); }, "synchronous runtime submit");
        require(mock->submissions == 1, "runtime did not submit exactly one entry");
        require(!Access::submitting(runtime), "submission reservation was not released");
        if (state == State::Acknowledged) {
            require(!Access::active(runtime).empty(), "ACK cleared ownership before a fill");
            require(Access::quantity(runtime) == 0, "ACK created an unconfirmed position");
        } else {
            require(Access::active(runtime).empty(), "terminal callback did not clear ownership");
            require((Access::quantity(runtime) > 0) == (state == State::Filled), "terminal position state is wrong");
        }
        if (state == State::Filled) {
            bounded([&] { Access::price(runtime, 110.0); }, "synchronous exit submit");
            require(mock->submissions == 2 && mock->latest.side == Side::Sell, "confirmed position did not submit its exit");
            require(Access::quantity(runtime) == 0, "confirmed exit did not clear position");
        }
        bounded([&] { runtime.stop(); }, "runtime stop after callback");
    }
}

void reservation_survives_terminal_callback_until_submit_returns() {
    Workspace workspace;
    auto venue = std::make_unique<SyncVenue>();
    auto* mock = venue.get();
    mock->result_state = State::Rejected;
    std::promise<void> terminal_seen, release;
    auto terminal = terminal_seen.get_future();
    auto released = release.get_future().share();
    mock->after_callback = [&] { terminal_seen.set_value(); released.wait(); };
    TestnetStrategyRuntime runtime("BTCUSDT", risk(), std::make_unique<BuyStrategy>(), std::move(venue));
    Access::start_without_stream(runtime);
    auto first = std::async(std::launch::async, [&] { Access::price(runtime, 100.0); });
    require_ready(terminal, "terminal callback");
    require(Access::active(runtime).empty(), "rejection did not release active identity");
    bounded([&] { Access::price(runtime, 101.0); }, "competing price tick");
    require(mock->submissions == 1, "competing tick submitted while prior submit was returning");
    release.set_value();
    require_ready(first, "first submission return");
    first.get();
    mock->after_callback = {};
    Access::price(runtime, 102.0);
    require(mock->submissions == 2, "reservation remained stuck after completed rejection");
}

void foreign_callback_does_not_release_reservation() {
    Workspace workspace;
    auto venue = std::make_unique<SyncVenue>();
    auto* mock = venue.get();
    TestnetStrategyRuntime runtime("BTCUSDT", risk(), std::make_unique<BuyStrategy>(), std::move(venue));
    Access::start_without_stream(runtime);
    Access::price(runtime, 100.0);
    const auto owner = Access::active(runtime);
    auto wrong = snapshot(mock->latest, State::Rejected);
    wrong.client_order_id = "another-order";
    mock->update(wrong);
    require(Access::active(runtime) == owner, "unrelated rejection released the active reservation");
    wrong = snapshot(mock->latest, State::Filled);
    wrong.symbol = "ETHUSDT";
    mock->update(wrong);
    require(Access::active(runtime) == owner && Access::quantity(runtime) == 0,
            "other-symbol fill changed reserved order state");
}

void submit_exception_blocks_followup_without_fabricating_rejection() {
    Workspace workspace;
    auto venue = std::make_unique<SyncVenue>();
    auto* mock = venue.get();
    mock->throw_on_submit = true;
    TestnetStrategyRuntime runtime("BTCUSDT", risk(), std::make_unique<BuyStrategy>(), std::move(venue));
    Access::start_without_stream(runtime);
    bounded([&] { Access::price(runtime, 100.0); }, "throwing venue");
    require(mock->killed() && mock->kills == 1, "submission exception did not latch one kill");
    require(!Access::active(runtime).empty(), "uncertain submission lost its reserved identity");
    require(Access::quantity(runtime) == 0, "submission exception invented a fill");
    Access::price(runtime, 101.0);
    require(mock->submissions == 1, "submission exception allowed another order");
    bounded([&] { runtime.stop(); }, "stop after submission exception");
}

void manager_observers_can_reenter() {
    for (const bool rejected : {false, true}) {
        MockExchange client;
        client.reject = rejected;
        sentum::order::OrderManager manager(client);
        manager.reconcile_startup();
        int updates = 0;
        manager.set_update_handler([&](const Snapshot& update) {
            ++updates;
            require(manager.get(update.client_order_id).has_value(), "observer could not inspect its order");
            if (update.state == State::Acknowledged || update.state == State::Rejected)
                manager.activate_kill_switch();
        });
        bounded([&] { manager.submit_market({"BTCUSDT", Side::Buy, 1, "reentrant-order"}); }, "manager observer re-entry");
        require(updates >= 2 && manager.kill_switch_active(), "reentrant observer did not complete");
        require(client.placements == 1, "manager repeated the REST submission");
    }
}

void observer_exception_is_not_exchange_rejection() {
    MockExchange client;
    sentum::order::OrderManager manager(client);
    manager.reconcile_startup();
    manager.set_update_handler([&](const Snapshot& update) {
        if (update.state == State::Acknowledged) throw std::runtime_error("observer failed");
    });
    bool failed = false;
    try { manager.submit_market({"BTCUSDT", Side::Buy, 1, "observer-failure"}); }
    catch (const std::runtime_error&) { failed = true; }
    require(failed, "observer failure was swallowed");
    require(manager.get("observer-failure")->state == State::Acknowledged,
            "observer error was converted into an exchange rejection");
}

void create_evidence_failure(const std::string& condition) {
    sqlite3* db = nullptr;
    require(sqlite3_open("log/klines.sqlite3", &db) == SQLITE_OK, "cannot open evidence fixture");
    const auto sql = "CREATE TRIGGER fail_order_evidence BEFORE INSERT ON order_events WHEN " + condition +
        " BEGIN SELECT RAISE(ABORT, 'evidence write failed'); END;";
    const int result = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr);
    sqlite3_close(db);
    require(result == SQLITE_OK, "cannot install evidence failure fixture");
}

void evidence_failure_can_kill_from_pending_and_ack() {
    for (const bool pending : {true, false}) {
        Workspace workspace;
        auto venue = std::make_unique<ManagedVenue>();
        auto* mock = venue.get();
        TestnetStrategyRuntime runtime("BTCUSDT", risk(), std::make_unique<BuyStrategy>(), std::move(venue));
        Access::start_without_stream(runtime);
        create_evidence_failure(pending ? "1" : "NEW.state != 'pending'");
        bounded([&] { Access::price(runtime, 100.0); }, "persistent evidence failure callback");
        require(mock->killed() && mock->kills == 1, "evidence failure did not latch one kill");
        require(mock->exchange.placements == (pending ? 0 : 1), "Pending kill allowed an exchange submission");
        require(Access::quantity(runtime) == 0, "failed evidence callback invented a fill");
        Access::price(runtime, 101.0);
        require(mock->exchange.placements == (pending ? 0 : 1), "failed persistence allowed a follow-up order");
        bounded([&] { runtime.stop(); }, "stop after persistence failure");
    }
}

void terminal_stream_update_is_not_republished_by_ack() {
    MockExchange client;
    sentum::order::OrderManager manager(client);
    manager.reconcile_startup();
    int fills = 0;
    manager.set_update_handler([&](const Snapshot& update) {
        if (update.state == State::Filled) ++fills;
        require(manager.get(update.client_order_id).has_value(), "fill observer cannot reenter manager");
    });
    client.during_placement = [&](const Request& request) {
        manager.on_user_data_event({{"e","executionReport"},{"c",request.client_order_id},
            {"s",request.symbol},{"i",42},{"S","BUY"},{"q","1"},{"z","1"},{"Z","100"},
            {"X","FILLED"},{"x","TRADE"},{"l","1"},{"t",1},{"n","0"},{"N",nullptr},
            {"E",1700000000000LL}});
    };
    bounded([&] {
        const auto result = manager.submit_market({"BTCUSDT", Side::Buy, 1, "filled-before-ack"});
        require(result.state == State::Filled, "late ACK regressed a synchronous terminal callback");
    }, "fill before ACK");
    require(fills == 1, "ACK republished a fill already delivered by the stream");
}
}

int main() {
    try {
        synchronous_runtime_states();
        reservation_survives_terminal_callback_until_submit_returns();
        foreign_callback_does_not_release_reservation();
        submit_exception_blocks_followup_without_fabricating_rejection();
        manager_observers_can_reenter();
        observer_exception_is_not_exchange_rejection();
        evidence_failure_can_kill_from_pending_and_ack();
        terminal_stream_update_is_not_republished_by_ack();
        std::cout << "testnet_callback_safety_tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "testnet_callback_safety_tests: " << error.what() << '\n';
        return 1;
    }
}
