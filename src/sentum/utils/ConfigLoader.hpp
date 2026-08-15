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

    nlohmann::json strategy = {
        {"type", "momentum"},
        {"parameters", {{"lookback", 20}, {"entry_threshold", 0.001}}}
    };

    double paperInitialBalance = 10000.0;
    std::string paperStatePath = "log/paper_account.json";
    bool paperAutoSymbol = true;
    std::string paperSymbol;
    std::string paperModelDefinition; // optional Phase-15 model JSON; registry stage must be paper

    std::string dashboardHost = "127.0.0.1";
    std::uint16_t dashboardPort = 8080;
};

Config load_config(const std::string& path);
