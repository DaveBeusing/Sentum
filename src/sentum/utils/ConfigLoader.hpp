/****
 * Copyright (C) 2025 Dave Beusing <david.beusing@gmail.com>
 * MIT License - https://opensource.org/license/mit/
 */
#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

struct Config {
    std::string quoteAsset = "USDC";
    std::string databasePath = "log/sentum.sqlite3";
    double minCumulativeReturn = 0.0005;
    bool paperTrading = true;

    // Runtime strategy configuration. Uses the same JSON shape as StrategyFactory/research.
    nlohmann::json strategy = {
        {"type", "momentum"},
        {"parameters", {{"lookback", 20}, {"entry_threshold", 0.001}}}
    };

    // Paper runtime/account configuration.
    double paperInitialBalance = 10000.0;
    std::string paperStatePath = "log/paper_account.json";
    bool paperAutoSymbol = true;
    std::string paperSymbol;

    // Dashboard configuration.
    std::string dashboardHost = "127.0.0.1";
    std::uint16_t dashboardPort = 8080;
};

Config load_config(const std::string& path);
