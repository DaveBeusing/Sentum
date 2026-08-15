/****
 * Copyright (C) 2025 Dave Beusing <david.beusing@gmail.com>
 * MIT License - https://opensource.org/license/mit/
 */

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>

#include <nlohmann/json.hpp>
#include <sentum/core/ExecutionEngine.hpp>
#include <sentum/core/RuntimeControl.hpp>
#include <sentum/dashboard/DashboardState.hpp>
#include <sentum/market/RuntimePerformanceMetrics.hpp>
#include <sentum/trader/strategy/StrategyFramework.hpp>
#include <sentum/trader/utils/RiskConfigLoader.hpp>

ExecutionEngine::ExecutionEngine()
    : running(false), collector_active(false), scanner_active(false), trader_active(false), logger("log/core.log") {
    std::filesystem::create_directories("log");
    logger.start();
}

ExecutionEngine::~ExecutionEngine() { stop(); logger.stop(); }
bool ExecutionEngine::is_running() const { return running.load(); }

void ExecutionEngine::start() {
    if (running.load()) return;
    init();
    running.store(true);
    sentum::dashboard::DashboardState::global().set("health", "healthy");
    main_thread = std::thread(&ExecutionEngine::run_main_loop, this);
}

void ExecutionEngine::stop() {
    const bool was_running = running.exchange(false);
    scanner_signal_cv.notify_all();
    if (!was_running && !main_thread.joinable() && !scanner_thread.joinable() && !trader_thread.joinable()) return;
    stop_trader();
    if (collector) { collector->stop(); collector_active.store(false); }
    if (main_thread.joinable() && main_thread.get_id() != std::this_thread::get_id()) main_thread.join();
    if (scanner_thread.joinable() && scanner_thread.get_id() != std::this_thread::get_id()) scanner_thread.join();
    scanner_active.store(false);
    sentum::dashboard::DashboardState::global().merge({
        {"collector_active", false}, {"scanner_active", false}, {"trader_active", false},
        {"market_data_connected", false}, {"health", "stopped"}
    });
}

void ExecutionEngine::init_config() {
    if (!std::filesystem::exists("config") || !std::filesystem::is_directory("config")) throw std::runtime_error("Required config/ directory is missing");
    if (!std::filesystem::exists("config/config.json")) throw std::runtime_error("Required config/config.json is missing");
    if (!std::filesystem::exists("config/secrets.json")) throw std::runtime_error("Required config/secrets.json is missing");
    if (!std::filesystem::exists("config/risk.json")) throw std::runtime_error("Required config/risk.json is missing");
    try { config = load_config("config/config.json"); }
    catch (const std::exception& e) { logger.log("[ERROR] Failed to load config/config.json: " + std::string(e.what())); throw; }
    try { secrets = load_secrets("config/secrets.json"); }
    catch (const std::exception& e) { logger.log("[ERROR] Failed to load config/secrets.json: " + std::string(e.what())); throw; }
    if (!config.paperTrading) throw std::runtime_error("Live trading is disabled. Set paperTrading=true in config/config.json");
    // Validate the configured strategy up front.
    (void)sentum::strategy::StrategyFactory::create(config.strategy);
    sentum::runtime::RuntimeControl::global().configure(config.strategy, config.paperAutoSymbol, config.paperSymbol);
    logger.log("[INFO] Running in PAPER TRADING mode");
}

void ExecutionEngine::init_components() {
    db_path = config.databasePath.empty() ? "log/klines.sqlite3" : config.databasePath;
    binance = std::make_unique<BinanceRestClient>(secrets.api_key, secrets.api_secret);
    markets = binance->get_markets_by_quote(config.quoteAsset);
    paper_account = std::make_unique<sentum::paper::PaperAccount>(config.paperStatePath, config.quoteAsset, config.paperInitialBalance);
    quote_balance = paper_account->equity();
    db = std::make_unique<Database>(db_path);
    market_store = std::make_unique<MarketDataStore>(600);
    collector = std::make_unique<Collector>(*db, *market_store, markets);
    scanner = std::make_unique<SymbolScanner>(*market_store, config.minCumulativeReturn);
    scanner->set_top_changed_handler([this](const SymbolPerformance& top) {
        if (!sentum::runtime::RuntimeControl::global().auto_symbol() || trader_active.load()) return;
        { std::lock_guard<std::mutex> lock(scanner_signal_mutex); pending_scanner_symbol = top.symbol; }
        scanner_signal_cv.notify_one();
    });
    collector->start();
    collector_active.store(true);
    scanner_active.store(true);

    if (!config.paperAutoSymbol && !config.paperSymbol.empty()) {
        std::lock_guard<std::mutex> lock(scanner_signal_mutex);
        pending_scanner_symbol = config.paperSymbol;
    }
}

void ExecutionEngine::init() {
    std::filesystem::create_directories("log");
    init_config();
    init_components();
    start_time = std::chrono::system_clock::now();
    applied_control_generation_ = sentum::runtime::RuntimeControl::global().generation();

    const auto strategy_json = sentum::runtime::RuntimeControl::global().strategy();
    sentum::dashboard::DashboardState::global().merge({
        {"mode", "paper"}, {"quote_asset", config.quoteAsset}, {"balance", quote_balance}, {"paper_account", paper_account->snapshot()},
        {"markets", markets.size()}, {"collector_active", true}, {"scanner_active", true}, {"trader_active", false},
        {"market_data_connected", true}, {"user_stream_connected", false}, {"reconciliation_complete", true}, {"kill_switch_active", false},
        {"db_path", db_path}, {"strategy_config", strategy_json}, {"strategy_name", strategy_json.value("type", std::string("momentum"))},
        {"symbol_mode", config.paperAutoSymbol ? "auto" : "manual"}, {"manual_symbol", config.paperSymbol}, {"entries_paused", false},
        {"started_at_ms", std::chrono::duration_cast<std::chrono::milliseconds>(start_time.time_since_epoch()).count()}
    });
}

void ExecutionEngine::apply_runtime_control() {
    auto& control = sentum::runtime::RuntimeControl::global();
    const auto generation = control.generation();
    if (generation == applied_control_generation_) return;

    bool position_open = trader && trader->get_current_position().open;
    if (position_open) {
        sentum::dashboard::DashboardState::global().set("control_pending", "waiting_for_position_exit");
        return;
    }

    const bool auto_symbol = control.auto_symbol();
    const auto manual_symbol = control.manual_symbol();
    const auto strategy_json = control.strategy();
    sentum::dashboard::DashboardState::global().merge({
        {"strategy_config", strategy_json}, {"strategy_name", strategy_json.value("type", std::string("momentum"))},
        {"symbol_mode", auto_symbol ? "auto" : "manual"}, {"manual_symbol", manual_symbol}, {"control_pending", nullptr}
    });

    // Recreate a running trader so the new strategy is applied atomically between positions.
    std::string restart_symbol;
    { std::lock_guard<std::mutex> lock(symbol_mutex); restart_symbol = current_symbol; }
    if (!auto_symbol && !manual_symbol.empty()) restart_symbol = manual_symbol;
    if (trader) stop_trader();
    if (!restart_symbol.empty()) start_trader_for(restart_symbol);
    applied_control_generation_ = generation;
}

void ExecutionEngine::run_main_loop() {
    using namespace std::chrono_literals;
    scanner_thread = std::thread(&ExecutionEngine::monitor_scanner, this);
    scanner_signal_cv.notify_all();
    auto last_db_probe = std::chrono::steady_clock::time_point{};
    auto last_event_sample = std::chrono::steady_clock::now();
    std::uint64_t previous_events = sentum::market::RuntimePerformanceMetrics::global().market_events.load(std::memory_order_relaxed);
    try {
        while (running.load()) {
            apply_runtime_control();
            std::string top_asset = "-"; double top_ret = 0.0;
            nlohmann::json scanner_json = nlohmann::json::array();
            if (scanner) {
                auto top = scanner->fetch_top_performers(60, 3);
                for (const auto& item : top) scanner_json.push_back({{"symbol", item.symbol}, {"return", item.cum_return}});
                if (!top.empty()) { top_asset = top[0].symbol; top_ret = top[0].cum_return * 100.0; }
            }

            const auto now = std::chrono::steady_clock::now();
            if (last_db_probe == std::chrono::steady_clock::time_point{} || now - last_db_probe >= 15s) {
                std::error_code ec;
                db_size = std::filesystem::exists(db_path, ec) ? std::filesystem::file_size(db_path, ec) : 0;
                if (ec) db_size = 0;
                last_db_probe = now;
            }

            std::string symbol_snapshot;
            { std::lock_guard<std::mutex> lock(symbol_mutex); symbol_snapshot = current_symbol; }
            auto& perf = sentum::market::RuntimePerformanceMetrics::global();
            const auto current_events = perf.market_events.load(std::memory_order_relaxed);
            const double sample_seconds = std::chrono::duration<double>(now - last_event_sample).count();
            const double events_per_second = sample_seconds > 0.0 ? static_cast<double>(current_events - previous_events) / sample_seconds : 0.0;
            previous_events = current_events; last_event_sample = now;

            nlohmann::json runtime = {
                {"scanner", scanner_json}, {"top_asset", top_asset}, {"top_return_percent", top_ret}, {"current_symbol", symbol_snapshot},
                {"db_size_bytes", db_size}, {"collector_active", collector_active.load()}, {"scanner_active", scanner_active.load()},
                {"trader_active", trader_active.load()}, {"drop_rate", collector ? collector->drop_rate() : 0.0},
                {"queue_depth", collector ? collector->queue_depth() : 0}, {"events_per_second", events_per_second},
                {"entries_paused", sentum::runtime::RuntimeControl::global().entries_paused()}, {"performance", perf.snapshot()}
            };

            if (trader) {
                const auto position = trader->get_current_position();
                const double total_profit = trader->get_total_profit();
                const double delta = total_profit - accounted_profit_;
                if (delta != 0.0 && paper_account) { paper_account->apply_realized_profit(delta); accounted_profit_ = total_profit; }
                quote_balance = paper_account ? paper_account->equity() : config.paperInitialBalance + total_profit;
                runtime["balance"] = quote_balance;
                runtime["paper_account"] = paper_account ? paper_account->snapshot() : nlohmann::json::object();
                runtime["total_profit"] = total_profit;
                runtime["total_trades"] = trader->get_total_trades();
                runtime["win_rate"] = trader->get_winrate_percent();
                runtime["wins"] = trader->get_win_count();
                runtime["losses"] = trader->get_lose_count();
                runtime["average_profit"] = trader->get_average_profit();
                runtime["strategy_name"] = trader->strategy_name();
                if (position.open) {
                    const double price = trader->get_latest_price();
                    const double profit = (price - position.entry_price) * position.quantity;
                    runtime["active_position"] = {{"symbol", position.symbol},{"entry_price",position.entry_price},{"quantity",position.quantity},
                        {"current_price",price},{"unrealized_profit",profit},{"stop_loss",position.stop_loss_price},{"take_profit",position.take_profit_price},
                        {"strategy",position.strategy},{"signal_reason",position.signal_reason},{"risk_reason",position.risk_reason}};
                } else runtime["active_position"] = nullptr;
            } else {
                runtime["balance"] = paper_account ? paper_account->equity() : config.paperInitialBalance;
                runtime["paper_account"] = paper_account ? paper_account->snapshot() : nlohmann::json::object();
                runtime["total_profit"] = 0.0; runtime["total_trades"] = 0; runtime["win_rate"] = 0.0;
                runtime["wins"] = 0; runtime["losses"] = 0; runtime["average_profit"] = 0.0; runtime["active_position"] = nullptr;
            }
            sentum::dashboard::DashboardState::global().merge(runtime);
            std::this_thread::sleep_for(1s);
        }
    } catch (const std::exception& e) {
        logger.log("[ERROR] ExecutionEngine::run_main_loop: " + std::string(e.what()));
        sentum::dashboard::DashboardState::global().merge({{"health", "error"}, {"last_error", e.what()}});
        running.store(false); scanner_signal_cv.notify_all();
    }
}

void ExecutionEngine::monitor_scanner() {
    try {
        while (running.load()) {
            std::string symbol;
            {
                std::unique_lock<std::mutex> lock(scanner_signal_mutex);
                scanner_signal_cv.wait(lock, [this] {
                    return !running.load() || !pending_scanner_symbol.empty() || !sentum::runtime::RuntimeControl::global().auto_symbol();
                });
                if (!running.load()) break;
                if (!sentum::runtime::RuntimeControl::global().auto_symbol()) {
                    symbol = sentum::runtime::RuntimeControl::global().manual_symbol();
                    pending_scanner_symbol.clear();
                } else symbol.swap(pending_scanner_symbol);
            }
            if (symbol.empty() || trader_active.load()) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); continue; }
            { std::lock_guard<std::mutex> lock(symbol_mutex); current_symbol = symbol; }
            start_trader_for(symbol);
        }
    } catch (const std::exception& e) { logger.log("[ERROR] ExecutionEngine::monitor_scanner: " + std::string(e.what())); running.store(false); }
}

void ExecutionEngine::start_trader_for(const std::string& symbol) {
    if (trader_thread.joinable()) trader_thread.join();
    auto risk = load_risk_config("config/risk.json");
    if (paper_account) risk.max_total_capital = paper_account->equity();
    auto strategy = sentum::strategy::StrategyFactory::create(sentum::runtime::RuntimeControl::global().strategy());
    trader = std::make_unique<TradeEngine>(symbol, *binance, risk, std::move(strategy), "log/klines.sqlite3");
    accounted_profit_ = 0.0;
    trader_active.store(true);
    sentum::dashboard::DashboardState::global().merge({{"current_symbol", symbol}, {"trader_active", true}});
    trader_thread = std::thread([this] {
        try { trader->run(); }
        catch (const std::exception& e) { logger.log("[ERROR] ExecutionEngine::start_trader_for: " + std::string(e.what())); }
        trader_active.store(false);
    });
}

void ExecutionEngine::stop_trader() {
    if (trader) {
        const double final_profit = trader->get_total_profit();
        const double delta = final_profit - accounted_profit_;
        if (delta != 0.0 && paper_account) paper_account->apply_realized_profit(delta);
        accounted_profit_ = 0.0;
        trader->stop();
    }
    if (trader_thread.joinable() && trader_thread.get_id() != std::this_thread::get_id()) trader_thread.join();
    trader.reset(); trader_active.store(false);
}
