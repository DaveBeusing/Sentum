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
#include <sentum/dashboard/DashboardState.hpp>
#include <sentum/market/RuntimePerformanceMetrics.hpp>

ExecutionEngine::ExecutionEngine()
    : running(false), collector_active(false), scanner_active(false), trader_active(false),
      quote_balance(0.0), db_size(0), logger("log/core.log") {
    std::filesystem::create_directories("log");
    logger.start();
}

ExecutionEngine::~ExecutionEngine() { stop(); logger.stop(); }
bool ExecutionEngine::is_running() const { return running.load(); }

void ExecutionEngine::start() {
    if (running.load()) return;
    init();
    ui->on_exit = [this]() { running.store(false); scanner_signal_cv.notify_all(); };
    ui->on_stop_trader = [this]() { stop_trader(); };
    ui->on_restart_collector = [this]() {
        if (collector) { collector->stop(); collector->start(); collector_active.store(true); ui->set_collector_active(true); }
    };
    running.store(true);
    sentum::dashboard::DashboardState::global().set("health", "healthy");
    ui_thread = std::thread([this] { ui->start(); });
    main_thread = std::thread(&ExecutionEngine::run_main_loop, this);
}

void ExecutionEngine::stop() {
    const bool was_running = running.exchange(false);
    scanner_signal_cv.notify_all();
    if (!was_running && !main_thread.joinable() && !ui_thread.joinable() && !scanner_thread.joinable() && !trader_thread.joinable()) return;
    stop_trader();
    if (collector) { collector->stop(); collector_active.store(false); }
    if (ui) ui->stop();
    if (main_thread.joinable() && main_thread.get_id() != std::this_thread::get_id()) main_thread.join();
    if (scanner_thread.joinable() && scanner_thread.get_id() != std::this_thread::get_id()) scanner_thread.join();
    if (ui_thread.joinable() && ui_thread.get_id() != std::this_thread::get_id()) ui_thread.join();
    scanner_active.store(false);
    sentum::dashboard::DashboardState::global().merge({
        {"collector_active", false}, {"scanner_active", false}, {"trader_active", false},
        {"market_data_connected", false}, {"health", "stopped"}
    });
    ui.reset();
}

void ExecutionEngine::init_config() {
    if (!std::filesystem::exists("config") || !std::filesystem::is_directory("config")) throw std::runtime_error("Required config/ directory is missing");
    if (!std::filesystem::exists("config/config.json")) throw std::runtime_error("Required config/config.json is missing");
    if (!std::filesystem::exists("config/secrets.json")) throw std::runtime_error("Required config/secrets.json is missing");
    if (!std::filesystem::exists("config/risk.json")) throw std::runtime_error("Required config/risk.json is missing");
    try { config = load_config("config/config.json"); }
    catch (const std::exception& e) { logger.log("[ERROR] Failed to load config/config.json: " + std::string(e.what())); throw; }
    try {
        secrets = load_secrets("config/secrets.json");
        if (secrets.api_key.empty() || secrets.api_secret.empty()) throw std::runtime_error("Missing API keys in secrets.json");
    } catch (const std::exception& e) { logger.log("[ERROR] Failed to load config/secrets.json: " + std::string(e.what())); throw; }
    if (!config.paperTrading) {
        logger.log("[ERROR] Live trading requested, but live order execution is not production-ready");
        throw std::runtime_error("Live trading is disabled. Set paperTrading=true in config/config.json");
    }
    logger.log("[INFO] Running in PAPER TRADING mode");
}

void ExecutionEngine::init_components() {
    db_path = "log/klines.sqlite3";
    binance = std::make_unique<BinanceRestClient>(secrets.api_key, secrets.api_secret);
    markets = binance->get_markets_by_quote(config.quoteAsset);
    quote_balance = binance->get_coin_balance(config.quoteAsset);
    db = std::make_unique<Database>(db_path);
    market_store = std::make_unique<MarketDataStore>(600);
    collector = std::make_unique<Collector>(*db, *market_store, markets);
    scanner = std::make_unique<SymbolScanner>(*market_store, config.minCumulativeReturn);
    scanner->set_top_changed_handler([this](const SymbolPerformance& top) {
        if (trader_active.load()) return;
        { std::lock_guard<std::mutex> lock(scanner_signal_mutex); pending_scanner_symbol = top.symbol; }
        scanner_signal_cv.notify_one();
    });
    ui = std::make_unique<UiConsole>();
    collector->start();
    collector_active.store(true);
    scanner_active.store(true);
}

void ExecutionEngine::init() {
    std::filesystem::create_directories("log");
    init_config();
    init_components();
    start_time = std::chrono::system_clock::now();
    ui->set_mode("PAPER TRADING");
    ui->set_collector_active(collector_active.load());
    ui->set_scanner_active(scanner_active.load());
    ui->set_trader_active(trader_active.load());
    ui->set_balance(quote_balance);
    ui->set_quote_asset(config.quoteAsset);
    ui->set_markets(markets.size());
    ui->set_start_time(start_time);
    ui->set_db_path(db_path);

    sentum::dashboard::DashboardState::global().merge({
        {"mode", "paper"}, {"quote_asset", config.quoteAsset}, {"balance", quote_balance},
        {"markets", markets.size()}, {"collector_active", true}, {"scanner_active", true},
        {"trader_active", false}, {"market_data_connected", true}, {"user_stream_connected", false},
        {"reconciliation_complete", true}, {"kill_switch_active", false}
    });
}

void ExecutionEngine::run_main_loop() {
    using namespace std::chrono_literals;
    scanner_thread = std::thread(&ExecutionEngine::monitor_scanner, this);
    auto last_db_probe = std::chrono::steady_clock::time_point{};
    auto last_event_sample = std::chrono::steady_clock::now();
    std::uint64_t previous_events = sentum::market::RuntimePerformanceMetrics::global().market_events.load(std::memory_order_relaxed);
    try {
        while (running.load()) {
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
            ui->set_top_performer(top_asset, top_ret);
            ui->set_countdown(0);
            ui->set_db_size(db_size);
            ui->set_collector_active(collector_active.load());
            ui->set_scanner_active(scanner_active.load());
            ui->set_trader_active(trader_active.load());
            ui->set_current_symbol(symbol_snapshot);

            auto& perf = sentum::market::RuntimePerformanceMetrics::global();
            const auto current_events = perf.market_events.load(std::memory_order_relaxed);
            const double sample_seconds = std::chrono::duration<double>(now - last_event_sample).count();
            const double events_per_second = sample_seconds > 0.0 ? static_cast<double>(current_events - previous_events) / sample_seconds : 0.0;
            previous_events = current_events;
            last_event_sample = now;

            nlohmann::json runtime = {
                {"scanner", scanner_json}, {"current_symbol", symbol_snapshot}, {"db_size_bytes", db_size},
                {"collector_active", collector_active.load()}, {"scanner_active", scanner_active.load()},
                {"trader_active", trader_active.load()}, {"drop_rate", collector ? collector->drop_rate() : 0.0},
                {"queue_depth", collector ? collector->queue_depth() : 0}, {"events_per_second", events_per_second},
                {"performance", perf.snapshot()}
            };

            if (trader) {
                const auto position = trader->get_current_position();
                const double total_profit = trader->get_total_profit();
                const int total_trades = trader->get_total_trades();
                const double win_rate = trader->get_winrate_percent();
                ui->set_trader_metrics(total_profit, trader->get_win_count(), trader->get_lose_count(), total_trades, win_rate, trader->get_average_profit());
                runtime["total_profit"] = total_profit;
                runtime["total_trades"] = total_trades;
                runtime["win_rate"] = win_rate;
                if (position.open) {
                    const double price = trader->get_latest_price();
                    const double profit = (price - position.entry_price) * position.quantity;
                    ui->set_active_trade(true, position.entry_price, position.quantity, position.stop_loss_price, position.take_profit_price, price, profit);
                    runtime["active_position"] = {{"symbol", position.symbol},{"entry_price",position.entry_price},{"quantity",position.quantity},{"current_price",price},{"unrealized_profit",profit},{"stop_loss",position.stop_loss_price},{"take_profit",position.take_profit_price}};
                } else {
                    ui->set_active_trade(false, 0, 0, 0, 0, 0, 0);
                    runtime["active_position"] = nullptr;
                }
            } else {
                runtime["total_profit"] = 0.0;
                runtime["total_trades"] = 0;
                runtime["win_rate"] = 0.0;
                runtime["active_position"] = nullptr;
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
                scanner_signal_cv.wait(lock, [this] { return !running.load() || !pending_scanner_symbol.empty(); });
                if (!running.load()) break;
                symbol.swap(pending_scanner_symbol);
            }
            if (symbol.empty() || trader_active.load()) continue;
            { std::lock_guard<std::mutex> lock(symbol_mutex); current_symbol = symbol; }
            start_trader_for(symbol);
        }
    } catch (const std::exception& e) { logger.log("[ERROR] ExecutionEngine::monitor_scanner: " + std::string(e.what())); running.store(false); }
}

void ExecutionEngine::start_trader_for(const std::string& symbol) {
    if (trader_thread.joinable()) trader_thread.join();
    trader = std::make_unique<TradeEngine>(symbol, *binance, true);
    trader_active.store(true);
    trader_thread = std::thread([this] {
        try { trader->run(); }
        catch (const std::exception& e) { logger.log("[ERROR] ExecutionEngine::start_trader_for: " + std::string(e.what())); }
        trader_active.store(false);
    });
}

void ExecutionEngine::stop_trader() {
    if (trader) trader->stop();
    if (trader_thread.joinable() && trader_thread.get_id() != std::this_thread::get_id()) trader_thread.join();
    trader.reset(); trader_active.store(false);
}
