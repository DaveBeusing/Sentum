/****
 * Copyright (C) 2025 Dave Beusing <david.beusing@gmail.com>
 * MIT License - https://opensource.org/license/mit/
 */
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <sentum/api/BinanceRestClient.hpp>
#include <sentum/collector/Collector.hpp>
#include <sentum/market/MarketDataStore.hpp>
#include <sentum/scanner/SymbolScanner.hpp>
#include <sentum/trader/TradeEngine.hpp>
#include <sentum/ui/UiConsole.hpp>
#include <sentum/utils/AsyncLogger.hpp>
#include <sentum/utils/ConfigLoader.hpp>
#include <sentum/utils/Database.hpp>
#include <sentum/utils/SecretsLoader.hpp>

class ExecutionEngine {
public:
    ExecutionEngine();
    ~ExecutionEngine();
    void start();
    void stop();
    bool is_running() const;

private:
    std::thread main_thread, ui_thread, scanner_thread, trader_thread;
    std::atomic<bool> running, collector_active, scanner_active, trader_active;
    std::mutex symbol_mutex;
    std::mutex scanner_signal_mutex;
    std::condition_variable scanner_signal_cv;
    std::string pending_scanner_symbol;

    std::unique_ptr<Database> db;
    std::unique_ptr<MarketDataStore> market_store;
    std::unique_ptr<BinanceRestClient> binance;
    std::unique_ptr<Collector> collector;
    std::unique_ptr<SymbolScanner> scanner;
    std::unique_ptr<TradeEngine> trader;
    std::unique_ptr<UiConsole> ui;

    std::string current_symbol;
    std::vector<MarketInfo> markets;
    double quote_balance;
    std::string db_path;
    size_t db_size;

    std::chrono::system_clock::time_point start_time;
    Config config;
    Secrets secrets;
    AsyncLogger logger;

    void init();
    void init_config();
    void init_components();
    void run_main_loop();
    void monitor_scanner();
    void start_trader_for(const std::string& symbol);
    void stop_trader();
};
