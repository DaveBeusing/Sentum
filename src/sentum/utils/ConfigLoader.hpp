/****
 * Copyright (C) 2025 Dave Beusing <david.beusing@gmail.com>
 * MIT License - https://opensource.org/license/mit/
 */
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>
#include <sentum/trader/types/RiskConfig.hpp>

namespace sentum::promotion { class ModelRevisionLease; }

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
    std::string paperModelDefinition;
    std::string paperRiskConfigPath = "config/risk.json";
    std::string paperModelId;
    std::int64_t paperModelRevision = 0;
    std::string paperModelDefinitionRevision;
    std::shared_ptr<sentum::promotion::ModelRevisionLease> paperModelLease;

    std::string dashboardHost = "127.0.0.1";
    std::uint16_t dashboardPort = 8080;
};

Config load_config(const std::string& path);
RiskConfig load_paper_risk_config(const Config& config);
void validate_paper_model_selection(const Config& config, const std::string& symbol,
                                    const nlohmann::json& strategy);
