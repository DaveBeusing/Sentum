#include <sentum/core/ExecutionEngine.hpp>
#include <sentum/core/RuntimeControl.hpp>
#include <sentum/dashboard/DashboardState.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>

namespace {
using namespace std::chrono_literals;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template<class T>
void require_ready(std::future<T>& result, const char* description) {
    if (result.wait_for(5s) != std::future_status::ready) {
        // A deadlocked worker must fail the subprocess without waiting in an
        // async future's destructor for the same deadlock to resolve.
        std::cerr << "trader_lifecycle_tests timed out: " << description << '\n';
        std::_Exit(2);
    }
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        std::string pattern = (std::filesystem::temp_directory_path() / "sentum-trader-lifecycle-XXXXXX").string();
        const char* created = ::mkdtemp(pattern.data());
        if (!created) throw std::runtime_error("Cannot create trader lifecycle fixture");
        path = created;
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
    std::filesystem::path path;
};

class BuyStrategy final : public IStrategy {
public:
    StrategySignal on_price(double price, std::chrono::system_clock::time_point now) override {
        return {TradeAction::BUY, "fixture", "entry", price, now, 1.0};
    }
    void reset() override {}
};

class GatedBuyStrategy final : public IStrategy {
public:
    GatedBuyStrategy(std::promise<void>& entered, std::shared_future<void> release)
        : entered_(entered), release_(std::move(release)) {}
    StrategySignal on_price(double price, std::chrono::system_clock::time_point now) override {
        entered_.set_value();
        release_.wait();
        return {TradeAction::BUY, "fixture", "entry", price, now, 1.0};
    }
    void reset() override {}
private:
    std::promise<void>& entered_;
    std::shared_future<void> release_;
};

std::unique_ptr<TradeEngine> replay_trader(std::unique_ptr<IStrategy> strategy = std::make_unique<BuyStrategy>()) {
    auto clock = std::make_shared<ReplayClock>();
    clock->advance_to(std::chrono::system_clock::now());
    return std::make_unique<TradeEngine>("BTCUSDT", RiskConfig{}, clock, std::move(strategy), ":memory:");
}

} // namespace

struct TradeEngineTestAccess {
    static bool cancellation_latched(const TradeEngine& engine) {
        return engine.stop_requested_.load(std::memory_order_acquire);
    }
    static bool untouched_startup(TradeEngine& engine) {
        std::lock_guard<std::mutex> lock(engine.queue_mutex);
        return !engine.run_started_ && !engine.running.load() && !engine.price_stream && !engine.history;
    }
    static void wait_for_stop(TradeEngine& engine) {
        std::unique_lock<std::mutex> lock(engine.queue_mutex);
        engine.queue_cv.wait(lock, [&engine] { return engine.stop_requested_.load(std::memory_order_acquire); });
    }
    static void run_queue(TradeEngine& engine) { engine.run_event_loop(); }
};

struct ExecutionEngineTestAccess {
    static void prepare(ExecutionEngine& engine, const std::filesystem::path& directory) {
        auto& control = sentum::runtime::RuntimeControl::global();
        control.configure({{"type", "momentum"}}, false, "BTCUSDT");
        control.pause_entries(false);
        (void)control.consume_manual_close();
        engine.applied_control_generation_ = control.generation();
        engine.current_symbol = "BTCUSDT";
        engine.trader = replay_trader();
        engine.trader_active.store(true);
        engine.running.store(true);
        engine.binance = std::make_unique<BinanceRestClient>("", "");
        // A replacement's worker fails before constructing a network stream.
        // This also verifies that a partial-start failure is retained by shutdown.
        engine.db_path = (directory / "absent" / "history.sqlite3").string();
        engine.config.paperRiskConfigPath = (directory / "risk.json").string();
        const nlohmann::json risk = {
            {"max_total_capital", 1000.0}, {"risk_per_trade", 0.01}, {"stop_loss_percent", 0.02},
            {"take_profit_percent", 0.04}, {"buy_fee_percent", 0.001}, {"sell_fee_percent", 0.001},
            {"slippage_percent", 0.0005}, {"spread_percent", 0.0002}, {"leverage", 1.0},
            {"min_quantity", 0.000001}, {"max_quantity", 0.0}, {"step_size", 0.000001},
            {"min_notional", 5.0}, {"cooldown_seconds", 30}, {"max_holding_seconds", 900},
            {"max_data_age_ms", 2000}
        };
        std::ofstream file(engine.config.paperRiskConfigPath);
        file << risk;
        require(file.good(), "Could not write lifecycle risk fixture");
    }

    static void gated_worker(ExecutionEngine& engine, std::promise<void>& stopped,
                             std::shared_future<void> release, std::atomic<int>& exited) {
        auto* const instance = engine.trader.get();
        engine.trader_thread = std::thread([&engine, instance, &stopped, release, &exited] {
            TradeEngineTestAccess::wait_for_stop(*instance);
            stopped.set_value();
            release.wait();
            exited.fetch_add(1);
            engine.trader_active.store(false, std::memory_order_release);
        });
    }

    static void scanner_start(ExecutionEngine& engine, const std::string& symbol) { engine.start_trader_for(symbol); }
    static void apply_control(ExecutionEngine& engine) { engine.apply_runtime_control(); }
    static TradeEngine* trader(ExecutionEngine& engine) { return engine.trader.get(); }
    static std::string symbol(ExecutionEngine& engine) {
        std::lock_guard<std::mutex> lock(engine.symbol_mutex);
        return engine.current_symbol;
    }
    static bool released(const ExecutionEngine& engine) {
        return !engine.trader && !engine.trader_thread.joinable() && !engine.trader_active.load();
    }
};

namespace {

void test_cancellation_before_worker_start_is_permanent() {
    TemporaryDirectory directory;
    BinanceRestClient api("", "");
    TradeEngine engine("BTCUSDT", api, RiskConfig{}, std::make_unique<BuyStrategy>(),
                       (directory.path / "unused.sqlite3").string());
    std::promise<void> release;
    auto gate = release.get_future().share();
    auto worker = std::async(std::launch::async, [&] { gate.wait(); engine.run(); });
    engine.stop();
    engine.stop();
    release.set_value();
    require_ready(worker, "cancellation before TradeEngine::run");
    worker.get();
    engine.run();
    require(TradeEngineTestAccess::cancellation_latched(engine), "run erased an earlier cancellation");
    require(TradeEngineTestAccess::untouched_startup(engine), "cancelled worker initialized runtime resources");
    require(!std::filesystem::exists(directory.path / "unused.sqlite3"), "cancelled startup created a database");
}

void test_flat_cancellation_prevents_late_entry() {
    auto engine = replay_trader();
    require(engine->try_stop_when_flat(), "flat trader rejected cancellation");
    require(engine->evaluate(100.0) == TradeAction::NONE, "cancelled trader accepted a late entry");
    require(!engine->get_current_position().open, "late event opened a position after cancellation");
}

void test_entry_and_flat_cancellation_are_serialized() {
    std::promise<void> entered, release;
    auto entered_future = entered.get_future();
    auto engine = replay_trader(std::make_unique<GatedBuyStrategy>(entered, release.get_future().share()));
    auto decision = std::async(std::launch::async, [&] { return engine->evaluate(100.0); });
    require_ready(entered_future, "entry decision gate");
    auto cancellation = std::async(std::launch::async, [&] { return engine->try_stop_when_flat(); });
    const bool completed_during_entry = cancellation.wait_for(50ms) == std::future_status::ready;
    release.set_value();
    require_ready(decision, "pending entry decision");
    require_ready(cancellation, "flat-only cancellation");
    require(decision.get() == TradeAction::BUY, "pending entry did not complete");
    require(!cancellation.get() && !completed_during_entry, "reconfiguration cancelled a concurrent open position");
    require(engine->get_current_position().open, "open position was discarded during reconfiguration");
    require(!TradeEngineTestAccess::cancellation_latched(*engine), "open-position rejection latched cancellation");
}

void test_idle_queue_observes_stop_without_another_event() {
    for (int cycle = 0; cycle < 32; ++cycle) {
        auto engine = replay_trader();
        auto worker = std::async(std::launch::async, [&] { TradeEngineTestAccess::run_queue(*engine); });
        engine->stop();
        require_ready(worker, "idle trader queue stop");
        worker.get();
    }
}

void test_scanner_does_not_join_an_active_trader() {
    TemporaryDirectory directory;
    ExecutionEngine engine;
    ExecutionEngineTestAccess::prepare(engine, directory.path);
    auto* const original = ExecutionEngineTestAccess::trader(engine);
    std::promise<void> stopped, release;
    std::atomic<int> exited{0};
    ExecutionEngineTestAccess::gated_worker(engine, stopped, release.get_future().share(), exited);
    auto scanner = std::async(std::launch::async, [&] { ExecutionEngineTestAccess::scanner_start(engine, "ETHUSDT"); });
    require_ready(scanner, "scanner must not join an active trader");
    scanner.get();
    require(ExecutionEngineTestAccess::trader(engine) == original, "scanner replaced an active trader");
    require(!TradeEngineTestAccess::cancellation_latched(*original), "scanner cancelled an active trader");
    require(ExecutionEngineTestAccess::symbol(engine) == "BTCUSDT", "rejected scanner request changed current symbol");
    release.set_value();
    engine.stop();
    require(exited.load() == 1 && ExecutionEngineTestAccess::released(engine), "active trader was not released");
}

void test_replacement_is_atomic_with_scanner_start_and_retains_worker_failure() {
    TemporaryDirectory directory;
    ExecutionEngine engine;
    ExecutionEngineTestAccess::prepare(engine, directory.path);
    std::promise<void> stopped, release;
    auto stopped_future = stopped.get_future();
    std::atomic<int> exited{0};
    ExecutionEngineTestAccess::gated_worker(engine, stopped, release.get_future().share(), exited);
    auto& control = sentum::runtime::RuntimeControl::global();
    control.set_strategy({{"type", "trend"}});
    control.set_manual_symbol("ETHUSDT");
    auto coordinator = std::async(std::launch::async, [&] { ExecutionEngineTestAccess::apply_control(engine); });
    require_ready(stopped_future, "coordinator must cancel before joining");

    std::promise<void> scanner_attempted;
    auto scanner_attempted_future = scanner_attempted.get_future();
    auto scanner = std::async(std::launch::async, [&] {
        scanner_attempted.set_value();
        ExecutionEngineTestAccess::scanner_start(engine, "STALEUSDT");
    });
    require_ready(scanner_attempted_future, "scanner contention start");
    const bool scanner_overtook = scanner.wait_for(50ms) == std::future_status::ready;
    release.set_value();
    require_ready(coordinator, "coordinator replacement");
    coordinator.get();
    require_ready(scanner, "scanner after replacement");
    scanner.get();
    require(!scanner_overtook, "scanner entered a replacement while the old trader was still shutting down");
    require(exited.load() == 1, "old worker survived replacement");
    require(ExecutionEngineTestAccess::symbol(engine) == "ETHUSDT", "replacement lost the requested manual symbol");
    require(ExecutionEngineTestAccess::trader(engine)->strategy_name() == "trend", "replacement lost the requested strategy");

    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (engine.is_running() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    require(!engine.is_running(), "worker initialization error did not stop the runtime");
    bool failed = false;
    try { engine.stop(); }
    catch (const std::runtime_error&) { failed = true; }
    require(failed, "shutdown concealed the worker failure");
    require(ExecutionEngineTestAccess::released(engine), "failed worker retained ownership after shutdown");
    require(sentum::dashboard::DashboardState::global().snapshot().at("health") == "error",
            "shutdown replaced worker failure with healthy completion");
}

void test_shutdown_during_replacement_cannot_launch_a_new_worker() {
    TemporaryDirectory directory;
    ExecutionEngine engine;
    ExecutionEngineTestAccess::prepare(engine, directory.path);
    std::promise<void> stopped, release;
    auto stopped_future = stopped.get_future();
    std::atomic<int> exited{0};
    ExecutionEngineTestAccess::gated_worker(engine, stopped, release.get_future().share(), exited);
    sentum::runtime::RuntimeControl::global().set_manual_symbol("ETHUSDT");
    auto coordinator = std::async(std::launch::async, [&] { ExecutionEngineTestAccess::apply_control(engine); });
    require_ready(stopped_future, "old worker cancellation before shutdown");
    auto shutdown = std::async(std::launch::async, [&] { engine.stop(); });
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (engine.is_running() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    const bool stopped_runtime = !engine.is_running();
    release.set_value();
    require_ready(coordinator, "replacement cancelled by shutdown");
    require_ready(shutdown, "shutdown during replacement");
    coordinator.get();
    shutdown.get();
    require(stopped_runtime, "shutdown failed to publish cancellation during replacement");
    require(exited.load() == 1 && ExecutionEngineTestAccess::released(engine), "shutdown retained a replacement worker");
    require(ExecutionEngineTestAccess::symbol(engine) == "BTCUSDT", "shutdown allowed the new symbol to start");
}

} // namespace

int main() {
    try {
        sentum::runtime::RuntimeControl::global().pause_entries(false);
        (void)sentum::runtime::RuntimeControl::global().consume_manual_close();
        test_cancellation_before_worker_start_is_permanent();
        test_flat_cancellation_prevents_late_entry();
        test_entry_and_flat_cancellation_are_serialized();
        test_idle_queue_observes_stop_without_another_event();
        test_scanner_does_not_join_an_active_trader();
        test_replacement_is_atomic_with_scanner_start_and_retains_worker_failure();
        test_shutdown_during_replacement_cannot_launch_a_new_worker();
        std::cout << "trader_lifecycle_tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "trader_lifecycle_tests failed: " << error.what() << '\n';
        return 1;
    }
}
