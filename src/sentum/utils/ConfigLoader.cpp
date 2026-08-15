/****
 * Copyright (C) 2025 Dave Beusing <david.beusing@gmail.com>
 * MIT License - https://opensource.org/license/mit/
 */

#include <fstream>
#include <stdexcept>

#include <nlohmann/json.hpp>
#include <sentum/utils/ConfigLoader.hpp>

Config load_config(const std::string& path) {
    Config config;
    std::ifstream file(path);
    if (!file.is_open()) throw std::runtime_error("can't open config file: " + path);

    nlohmann::json json;
    file >> json;
    config.quoteAsset = json.value("quoteAsset", config.quoteAsset);
    config.minCumulativeReturn = json.value("minCumulativeReturn", config.minCumulativeReturn);
    config.databasePath = json.value("databasePath", config.databasePath);
    config.paperTrading = json.value("paperTrading", config.paperTrading);
    config.strategy = json.value("strategy", config.strategy);

    if (json.contains("paper") && json.at("paper").is_object()) {
        const auto& paper = json.at("paper");
        config.paperInitialBalance = paper.value("initialBalance", config.paperInitialBalance);
        config.paperStatePath = paper.value("statePath", config.paperStatePath);
        config.paperAutoSymbol = paper.value("autoSymbol", config.paperAutoSymbol);
        config.paperSymbol = paper.value("symbol", config.paperSymbol);
        config.paperModelDefinition = paper.value("modelDefinition", config.paperModelDefinition);
    }

    config.dashboardHost = json.value("dashboardHost", config.dashboardHost);
    const int dashboard_port = json.value("dashboardPort", static_cast<int>(config.dashboardPort));
    if (dashboard_port < 1 || dashboard_port > 65535) throw std::runtime_error("dashboardPort must be between 1 and 65535");
    config.dashboardPort = static_cast<std::uint16_t>(dashboard_port);
    if (config.dashboardHost.empty()) throw std::runtime_error("dashboardHost must not be empty");
    if (!(config.paperInitialBalance > 0.0)) throw std::runtime_error("paper.initialBalance must be > 0");
    if (!config.strategy.is_object()) throw std::runtime_error("strategy must be a JSON object");
    if (!config.paperAutoSymbol && config.paperSymbol.empty() && config.paperModelDefinition.empty())
        throw std::runtime_error("paper.symbol is required when paper.autoSymbol=false unless paper.modelDefinition is set");
    return config;
}
