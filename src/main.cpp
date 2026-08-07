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
#include <sentum/dashboard/DashboardServer.hpp>
#include <sentum/dashboard/DashboardState.hpp>
#include <sentum/research/ResearchPlatform.hpp>
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

std::uint16_t dashboard_port() {
    const char* value = std::getenv("SENTUM_DASHBOARD_PORT");
    if (!value || !*value) return 8080;
    try {
        const int parsed = std::stoi(value);
        if (parsed < 1 || parsed > 65535) throw std::out_of_range("port");
        return static_cast<std::uint16_t>(parsed);
    } catch (...) {
        throw std::runtime_error("SENTUM_DASHBOARD_PORT must be between 1 and 65535");
    }
}

std::unique_ptr<sentum::dashboard::DashboardServer> start_dashboard() {
    auto server = std::make_unique<sentum::dashboard::DashboardServer>(dashboard_port());
    server->start();
    std::cout << "Sentum Dashboard: http://127.0.0.1:" << server->port() << "\n";
    return server;
}

ReplayResult run_replay(const std::string& path, const std::string& symbol, RiskConfig risk, const std::string& history_path) {
    const auto events = HistoricalEventReader::read_csv(path, symbol);
    if (events.empty()) throw std::runtime_error("Replay input contains no events");
    auto clock = std::make_shared<ReplayClock>();
    TradeEngine engine(symbol, risk, clock, std::make_unique<MomentumStrategy>(), history_path);
    for (const auto& event : events) { clock->advance_to(event.timestamp); engine.process_event(event); }
    return {MetricsCalculator::calculate(engine.completed_trades()), engine.get_total_profit()};
}

int replay_main(const std::string& path, const std::string& symbol) {
    std::filesystem::create_directories("log");
    std::filesystem::remove("log/replay.sqlite3");
    RiskConfig base = load_risk_config("config/risk.json");
    const auto baseline = run_replay(path, symbol, base, "log/replay.sqlite3");
    RiskConfig stressed = base;
    stressed.slippage_percent = base.slippage_percent * 2.0;
    const auto stress = run_replay(path, symbol, stressed, ":memory:");
    auto metrics = baseline.metrics;
    metrics.slippage_sensitivity = stress.net_profit - baseline.net_profit;
    std::cout << std::fixed << std::setprecision(6)
              << "Replay symbol: " << symbol << '\n' << "Trades: " << metrics.trades << '\n'
              << "Net Profit: " << metrics.net_profit << '\n' << "Max Drawdown: " << metrics.max_drawdown << '\n'
              << "Profit Factor: " << metrics.profit_factor << '\n' << "Win Rate (%): " << metrics.win_rate << '\n'
              << "Expectancy: " << metrics.expectancy << '\n' << "Sharpe: " << metrics.sharpe << '\n'
              << "Sortino: " << metrics.sortino << '\n' << "Fee Share: " << metrics.fee_share << '\n'
              << "Slippage Sensitivity (2x - baseline): " << metrics.slippage_sensitivity << '\n';
    nlohmann::json report{{"symbol",symbol},{"trades",metrics.trades},{"net_profit",metrics.net_profit},
        {"max_drawdown",metrics.max_drawdown},{"profit_factor",metrics.profit_factor},{"win_rate",metrics.win_rate},
        {"expectancy",metrics.expectancy},{"sharpe",metrics.sharpe},{"sortino",metrics.sortino},
        {"fee_share",metrics.fee_share},{"slippage_sensitivity",metrics.slippage_sensitivity}};
    std::ofstream("log/replay_metrics.json") << report.dump(2) << '\n';
    auto& dashboard = sentum::dashboard::DashboardState::global();
    dashboard.set("mode", "replay");
    dashboard.set("health", "complete");
    dashboard.set("current_symbol", symbol);
    dashboard.set("total_profit", metrics.net_profit);
    dashboard.set("total_trades", metrics.trades);
    dashboard.set("win_rate", metrics.win_rate);
    return EXIT_SUCCESS;
}

int research_main(const std::string& config_path) {
    const auto config = sentum::research::load_research_config(config_path);
    const RiskConfig risk = load_risk_config("config/risk.json");
    sentum::research::ResearchRunner runner(risk);

    std::cout << "Quant research: " << config.symbol << "\n"
              << "Dataset: " << config.dataset << "\n"
              << "Objective: " << config.objective << "\n";

    const auto summary = runner.run(config);
    sentum::research::ResearchRunner::write_artifacts(summary);

    std::cout << "Events: " << summary.events << "\n"
              << "Walk-forward folds: " << summary.folds << "\n"
              << "Trials: " << summary.trials << "\n";
    if (!summary.leaderboard.empty()) {
        const auto& best = summary.leaderboard.front();
        std::cout << std::fixed << std::setprecision(8)
                  << "Best trial: " << best.trial_id << "\n"
                  << "Validation score: " << best.validation_score << "\n"
                  << "Train score: " << best.train_score << "\n"
                  << "Overfit gap: " << best.overfit_gap << "\n"
                  << "Lookback: " << best.parameters.lookback << "\n"
                  << "Entry threshold: " << best.parameters.entry_threshold << "\n"
                  << "Stop loss: " << best.parameters.stop_loss_percent << "\n"
                  << "Take profit: " << best.parameters.take_profit_percent << "\n"
                  << "Slippage: " << best.parameters.slippage_percent << "\n"
                  << "Validation trades: " << best.validation.trades << "\n"
                  << "Validation net profit: " << best.validation.net_profit << "\n"
                  << "Validation max drawdown: " << best.validation.max_drawdown << "\n";
    }
    std::cout << "Research artifacts: log/research_latest.json, log/research_trials.csv\n";

    auto& dashboard = sentum::dashboard::DashboardState::global();
    dashboard.set("mode", "research");
    dashboard.set("health", "complete");
    dashboard.set("current_symbol", config.symbol);
    dashboard.set("research_trials", summary.trials);
    if (!summary.leaderboard.empty()) dashboard.set("research_best_score", summary.leaderboard.front().validation_score);
    return EXIT_SUCCESS;
}

int paper_main() {
    auto dashboard = start_dashboard();
    auto engine = std::make_unique<ExecutionEngine>();
    engine->start();
    while (engine->is_running() && !shutdown_requested.load(std::memory_order_relaxed)) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    engine->stop();
    dashboard->stop();
    return EXIT_SUCCESS;
}

int testnet_main(const std::string& symbol) {
    auto dashboard_server = start_dashboard();
    auto& dashboard = sentum::dashboard::DashboardState::global();
    dashboard.set("mode", "testnet");
    dashboard.set("symbol", symbol);
    dashboard.set("health", "starting");
    RiskConfig risk = load_risk_config("config/risk.json");
    auto venue = std::make_unique<sentum::execution::BinanceTestnetExecutionVenue>();
    sentum::execution::TestnetStrategyRuntime runtime(symbol, risk, std::make_unique<MomentumStrategy>(), std::move(venue));
    runtime.start();
    dashboard.set("health", "healthy");
    while (runtime.running() && !shutdown_requested.load(std::memory_order_relaxed)) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    runtime.stop();
    dashboard.set("health", "stopped");
    dashboard_server->stop();
    return EXIT_SUCCESS;
}

int dashboard_main() {
    auto dashboard = start_dashboard();
    std::cout << "Read-only dashboard mode. Press Ctrl+C to stop.\n";
    while (!shutdown_requested.load(std::memory_order_relaxed)) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    dashboard->stop();
    return EXIT_SUCCESS;
}

void usage() {
    std::cerr << "Usage:\n"
              << "  client --paper\n"
              << "  client --replay <timestamp_ms,price,volume.csv> <symbol>\n"
              << "  client --research <research.json>\n"
              << "  client --testnet <symbol>\n"
              << "  client --dashboard\n\n"
              << "Optional environment:\n  SENTUM_DASHBOARD_PORT=8080\n";
}
}

int main(int argc, char** argv) {
    std::signal(SIGINT, handle_signal); std::signal(SIGTERM, handle_signal);
    try {
        if (argc == 1 || (argc == 2 && std::string(argv[1]) == "--paper")) return paper_main();
        if (argc == 4 && std::string(argv[1]) == "--replay") return replay_main(argv[2], argv[3]);
        if (argc == 3 && std::string(argv[1]) == "--research") return research_main(argv[2]);
        if (argc == 3 && std::string(argv[1]) == "--testnet") return testnet_main(argv[2]);
        if (argc == 2 && std::string(argv[1]) == "--dashboard") return dashboard_main();
        usage(); return EXIT_FAILURE;
    } catch (const std::exception& e) { std::cerr << "[FATAL] " << e.what() << '\n'; }
      catch (...) { std::cerr << "[FATAL] Unknown unhandled exception\n"; }
    return EXIT_FAILURE;
}
