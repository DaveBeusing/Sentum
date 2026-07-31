/****
 * Copyright (C) 2025 Dave Beusing <david.beusing@gmail.com>
 * MIT License - https://opensource.org/license/mit/
 */

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>
#include <sentum/backtest/Backtest.hpp>
#include <sentum/core/ExecutionEngine.hpp>
#include <sentum/time/Clock.hpp>
#include <sentum/trader/TradeEngine.hpp>
#include <sentum/trader/execution/BinanceTestnetExecutionVenue.hpp>
#include <sentum/trader/execution/TestnetStrategyRuntime.hpp>
#include <sentum/trader/strategy/MomentumStrategy.hpp>
#include <sentum/trader/utils/RiskConfigLoader.hpp>

namespace {
std::atomic<bool> shutdown_requested{false};
void handle_signal(int) noexcept { shutdown_requested.store(true, std::memory_order_relaxed); }

struct ReplayResult { BacktestMetrics metrics; double net_profit = 0.0; };

ReplayResult run_replay(const std::string& path, const std::string& symbol, RiskConfig risk) {
    const auto events = HistoricalEventReader::read_csv(path, symbol);
    if (events.empty()) throw std::runtime_error("Replay input contains no events");
    auto clock = std::make_shared<ReplayClock>();
    TradeEngine engine(symbol, risk, clock, std::make_unique<MomentumStrategy>(), ":memory:");
    for (const auto& event : events) { clock->advance_to(event.timestamp); engine.process_event(event); }
    return {MetricsCalculator::calculate(engine.completed_trades()), engine.get_total_profit()};
}

int replay_main(const std::string& path, const std::string& symbol) {
    RiskConfig base = load_risk_config("config/risk.json");
    const auto baseline = run_replay(path, symbol, base);
    RiskConfig stressed = base;
    stressed.slippage_percent = base.slippage_percent * 2.0;
    const auto stress = run_replay(path, symbol, stressed);
    auto metrics = baseline.metrics;
    metrics.slippage_sensitivity = stress.net_profit - baseline.net_profit;

    std::cout << std::fixed << std::setprecision(6)
              << "Replay symbol: " << symbol << '\n'
              << "Trades: " << metrics.trades << '\n'
              << "Net Profit: " << metrics.net_profit << '\n'
              << "Max Drawdown: " << metrics.max_drawdown << '\n'
              << "Profit Factor: " << metrics.profit_factor << '\n'
              << "Win Rate (%): " << metrics.win_rate << '\n'
              << "Expectancy: " << metrics.expectancy << '\n'
              << "Sharpe: " << metrics.sharpe << '\n'
              << "Sortino: " << metrics.sortino << '\n'
              << "Fee Share: " << metrics.fee_share << '\n'
              << "Slippage Sensitivity (2x - baseline): " << metrics.slippage_sensitivity << '\n';

    std::filesystem::create_directories("log");
    nlohmann::json report{{"symbol",symbol},{"trades",metrics.trades},{"net_profit",metrics.net_profit},
        {"max_drawdown",metrics.max_drawdown},{"profit_factor",metrics.profit_factor},{"win_rate",metrics.win_rate},
        {"expectancy",metrics.expectancy},{"sharpe",metrics.sharpe},{"sortino",metrics.sortino},
        {"fee_share",metrics.fee_share},{"slippage_sensitivity",metrics.slippage_sensitivity}};
    std::ofstream("log/replay_metrics.json") << report.dump(2) << '\n';
    return EXIT_SUCCESS;
}

int paper_main() {
    auto engine = std::make_unique<ExecutionEngine>();
    engine->start();
    while (engine->is_running() && !shutdown_requested.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    engine->stop();
    return EXIT_SUCCESS;
}

int testnet_main(const std::string& symbol) {
    RiskConfig risk = load_risk_config("config/risk.json");
    auto venue = std::make_unique<sentum::execution::BinanceTestnetExecutionVenue>();
    sentum::execution::TestnetStrategyRuntime runtime(
        symbol, risk, std::make_unique<MomentumStrategy>(), std::move(venue));
    runtime.start();
    while (runtime.running() && !shutdown_requested.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    runtime.stop();
    return EXIT_SUCCESS;
}

void usage() {
    std::cerr << "Usage:\n"
              << "  client --paper\n"
              << "  client --replay <timestamp_ms,price,volume.csv> <symbol>\n"
              << "  client --testnet <symbol>\n";
}
}

int main(int argc, char** argv) {
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);
    try {
        if (argc == 1 || (argc == 2 && std::string(argv[1]) == "--paper")) return paper_main();
        if (argc == 4 && std::string(argv[1]) == "--replay") return replay_main(argv[2], argv[3]);
        if (argc == 3 && std::string(argv[1]) == "--testnet") return testnet_main(argv[2]);
        usage();
        return EXIT_FAILURE;
    } catch (const std::exception& e) {
        std::cerr << "[FATAL] " << e.what() << '\n';
    } catch (...) {
        std::cerr << "[FATAL] Unknown unhandled exception\n";
    }
    return EXIT_FAILURE;
}
