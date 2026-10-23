#include <sentum/core/ExecutionEngine.hpp>
#include <sentum/core/RuntimeControl.hpp>
#include <sentum/dashboard/DashboardState.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

class BuyStrategy final : public IStrategy {
public:
    StrategySignal on_price(double price, std::chrono::system_clock::time_point now) override {
        return {TradeAction::BUY, "fixture", "entry", price, now, 1.0};
    }
    void reset() override {}
};

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        std::string pattern = (std::filesystem::temp_directory_path() / "sentum-account-runtime-XXXXXX").string();
        char* created = ::mkdtemp(pattern.data());
        if (!created) throw std::runtime_error("Cannot create account runtime fixture directory");
        path = created;
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
    std::filesystem::path path;
};

std::string read_file(const std::filesystem::path& path) {
    std::ifstream stream(path);
    if (!stream) throw std::runtime_error("Cannot read account fixture");
    return std::string(std::istreambuf_iterator<char>(stream), {});
}

} // namespace

struct ExecutionEngineTestAccess {
    static void prepare(ExecutionEngine& engine, const std::filesystem::path& path,
                        std::atomic<int>& workers_exited, bool start_fixture_workers = true) {
        engine.paper_account = std::make_unique<sentum::paper::PaperAccount>(path.string(), "USDT", 1000.0);
        auto clock = std::make_shared<ReplayClock>();
        const auto now = std::chrono::system_clock::now();
        clock->advance_to(now);
        RiskConfig risk;
        risk.buy_fee_percent = risk.sell_fee_percent = 0.0;
        risk.slippage_percent = risk.spread_percent = 0.0;
        engine.trader = std::make_unique<TradeEngine>("BTCUSDT", risk, clock, std::make_unique<BuyStrategy>(), ":memory:");
        require(engine.trader->evaluate(100.0) == TradeAction::BUY, "fixture entry was rejected");
        clock->advance_to(now + std::chrono::seconds(1));
        require(engine.trader->evaluate(110.0) == TradeAction::SELL, "fixture exit was rejected");
        require(engine.trader->get_total_profit() == 50.0, "fixture should realize a profit of 50");

        engine.running.store(true);
        engine.trader_active.store(true);
        if (!start_fixture_workers) return;
        auto worker = [&engine, &workers_exited] {
            std::unique_lock<std::mutex> lock(engine.shutdown_wait_mutex);
            engine.shutdown_wait_cv.wait(lock, [&engine] { return !engine.running.load(); });
            workers_exited.fetch_add(1);
        };
        engine.main_thread = std::thread(worker);
        engine.scanner_thread = std::thread(worker);
        engine.trader_thread = std::thread(worker);
    }

    static void start_control_change(ExecutionEngine& engine) {
        auto& control = sentum::runtime::RuntimeControl::global();
        control.configure({{"type", "momentum"}}, true, "");
        engine.applied_control_generation_ = control.generation();
        control.set_strategy({{"type", "trend"}});
        engine.main_thread = std::thread(&ExecutionEngine::run_main_loop, &engine);
    }

    static bool released(const ExecutionEngine& engine) {
        return !engine.main_thread.joinable() && !engine.scanner_thread.joinable() &&
               !engine.trader_thread.joinable() && !engine.trader && !engine.trader_active.load();
    }

    static double equity(const ExecutionEngine& engine) { return engine.paper_account->equity(); }
};

namespace {

void test_successful_shutdown_persists_final_profit() {
    TemporaryDirectory directory;
    const auto path = directory.path / "account.json";
    std::atomic<int> exited{0};
    ExecutionEngine engine;
    ExecutionEngineTestAccess::prepare(engine, path, exited);
    engine.stop();
    require(ExecutionEngineTestAccess::released(engine), "shutdown retained a worker or trader");
    require(exited.load() == 3, "shutdown did not join every worker");
    sentum::paper::PaperAccount reopened(path.string(), "USDT", 1000.0);
    require(reopened.equity() == 1050.0, "final realized profit was not durable");
    engine.stop();
    sentum::paper::PaperAccount reopened_after_stop(path.string(), "USDT", 1000.0);
    require(reopened_after_stop.equity() == 1050.0, "repeated stop changed account state");
}

void test_failed_persistence_still_stops_all_workers() {
    TemporaryDirectory directory;
    const auto path = directory.path / "account.json";
    const auto backup = directory.path / "last-good.json";
    std::atomic<int> exited{0};
    ExecutionEngine engine;
    ExecutionEngineTestAccess::prepare(engine, path, exited);
    const auto original = read_file(path);
    std::filesystem::rename(path, backup);
    std::filesystem::create_directory(path);

    bool failed = false;
    std::size_t last_step = 0;
    try {
        engine.stop([&](std::size_t step, std::size_t, const std::string&) { last_step = step; });
    } catch (const std::exception&) {
        failed = true;
    }
    require(failed, "shutdown concealed failed account persistence");
    require(last_step == 6, "shutdown stopped before releasing its remaining subsystems");
    require(ExecutionEngineTestAccess::released(engine), "failed persistence left a joinable worker");
    require(exited.load() == 3, "a worker survived failed persistence");
    require(ExecutionEngineTestAccess::equity(engine) == 1000.0, "failed write committed account memory");
    require(read_file(backup) == original, "failed shutdown changed the last good account bytes");
    require(std::filesystem::is_directory(path), "shutdown deleted an obstructing filesystem object");
    const auto status = sentum::dashboard::DashboardState::global().snapshot();
    require(status.at("health") == "error", "shutdown replaced persistence failure with healthy completion");
    require(!status.value("last_error", std::string{}).empty(), "shutdown omitted persistence diagnostics");
    engine.stop();
}

void test_control_change_failure_survives_shutdown() {
    TemporaryDirectory directory;
    const auto path = directory.path / "account.json";
    const auto backup = directory.path / "last-good.json";
    std::atomic<int> unused_workers{0};
    ExecutionEngine engine;
    ExecutionEngineTestAccess::prepare(engine, path, unused_workers, false);
    const auto original = read_file(path);
    std::filesystem::rename(path, backup);
    std::filesystem::create_directory(path);
    ExecutionEngineTestAccess::start_control_change(engine);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (engine.is_running() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(!engine.is_running(), "runtime did not stop after failed control-change persistence");

    bool failed = false;
    try {
        engine.stop();
    } catch (const sentum::paper::PaperAccount::PersistenceError&) {
        failed = true;
    }
    require(failed, "shutdown concealed an earlier control-change persistence failure");
    require(ExecutionEngineTestAccess::released(engine), "control-change failure retained runtime workers");
    require(ExecutionEngineTestAccess::equity(engine) == 1000.0, "failed control change committed account memory");
    require(read_file(backup) == original, "failed control change modified the last good account");
    const auto status = sentum::dashboard::DashboardState::global().snapshot();
    require(status.at("health") == "error", "shutdown cleared the earlier persistence failure");
    require(!status.value("last_error", std::string{}).empty(), "earlier persistence diagnostics were lost");
}

void test_destructor_contains_persistence_failure_after_cleanup() {
    TemporaryDirectory directory;
    const auto path = directory.path / "account.json";
    const auto backup = directory.path / "last-good.json";
    std::atomic<int> exited{0};
    {
        ExecutionEngine engine;
        ExecutionEngineTestAccess::prepare(engine, path, exited);
        std::filesystem::rename(path, backup);
        std::filesystem::create_directory(path);
    }
    require(exited.load() == 3, "destructor did not finish cleanup after persistence failure");
    sentum::paper::PaperAccount reopened(backup.string(), "USDT", 1000.0);
    require(reopened.equity() == 1000.0, "destructor damaged the original account");
}

void test_control_change_account_limit_survives_shutdown() {
    TemporaryDirectory directory;
    const auto path = directory.path / "account.json";
    const nlohmann::json limited_state = {
        {"currency", "USDT"}, {"initial_balance", 1000.0}, {"equity", 1000.0},
        {"realized_profit", 0.0}, {"closed_trades", std::numeric_limits<int>::max()}
    };
    {
        std::ofstream file(path);
        file << limited_state;
        require(file.good(), "could not write account-limit fixture");
    }
    const auto original = read_file(path);
    std::atomic<int> unused_workers{0};
    ExecutionEngine engine;
    ExecutionEngineTestAccess::prepare(engine, path, unused_workers, false);
    ExecutionEngineTestAccess::start_control_change(engine);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (engine.is_running() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(!engine.is_running(), "account limit did not stop the runtime");
    bool failed = false;
    try { engine.stop(); }
    catch (const std::invalid_argument&) { failed = true; }
    require(failed, "shutdown concealed failed account finalization at its numeric limit");
    require(ExecutionEngineTestAccess::released(engine), "account-limit failure retained runtime workers");
    require(read_file(path) == original, "account-limit failure changed the persisted state");
    require(sentum::dashboard::DashboardState::global().snapshot().at("health") == "error",
            "shutdown cleared the account-limit error");
}

} // namespace

int main() {
    try {
        test_successful_shutdown_persists_final_profit();
        test_failed_persistence_still_stops_all_workers();
        test_control_change_failure_survives_shutdown();
        test_control_change_account_limit_survives_shutdown();
        test_destructor_contains_persistence_failure_after_cleanup();
        std::cout << "paper_account_runtime_tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "paper_account_runtime_tests failed: " << error.what() << '\n';
        return 1;
    }
}
