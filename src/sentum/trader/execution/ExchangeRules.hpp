#pragma once

#include <cmath>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

namespace sentum::execution {

struct ExchangeRules {
    double min_quantity = 0.0;
    double max_quantity = 0.0;
    double step_size = 0.0;
    double min_notional = 0.0;
    int base_precision = 8;
    int quote_precision = 8;

    double normalize_quantity(double value) const {
        if (step_size <= 0.0) throw std::logic_error("Invalid exchange step size");
        const double normalized = std::floor(value / step_size) * step_size;
        if (normalized < min_quantity) throw std::runtime_error("Quantity below exchange minimum");
        if (max_quantity > 0.0 && normalized > max_quantity) throw std::runtime_error("Quantity above exchange maximum");
        return normalized;
    }

    void validate_notional(double quantity, double price) const {
        if (quantity * price < min_notional) throw std::runtime_error("Order notional below exchange minimum");
    }

    static ExchangeRules from_exchange_info(const nlohmann::json& info, const std::string& symbol) {
        if (!info.contains("symbols") || !info.at("symbols").is_array()) throw std::runtime_error("Invalid exchangeInfo response");
        for (const auto& item : info.at("symbols")) {
            if (item.value("symbol", std::string{}) != symbol) continue;
            if (item.value("status", std::string{}) != "TRADING") throw std::runtime_error("Symbol is not trading");
            ExchangeRules rules;
            rules.base_precision = item.value("baseAssetPrecision", 8);
            rules.quote_precision = item.value("quoteAssetPrecision", 8);
            for (const auto& filter : item.at("filters")) {
                const auto type = filter.value("filterType", std::string{});
                if (type == "LOT_SIZE" || type == "MARKET_LOT_SIZE") {
                    const auto parse = [&](const char* key) { return std::stod(filter.value(key, std::string{"0"})); };
                    if (rules.min_quantity == 0.0 || type == "MARKET_LOT_SIZE") rules.min_quantity = parse("minQty");
                    if (rules.max_quantity == 0.0 || type == "MARKET_LOT_SIZE") rules.max_quantity = parse("maxQty");
                    if (rules.step_size == 0.0 || type == "MARKET_LOT_SIZE") rules.step_size = parse("stepSize");
                } else if (type == "MIN_NOTIONAL" || type == "NOTIONAL") {
                    rules.min_notional = std::stod(filter.value("minNotional", std::string{"0"}));
                }
            }
            if (rules.step_size <= 0.0) throw std::runtime_error("Exchange did not provide a usable quantity step");
            return rules;
        }
        throw std::runtime_error("Symbol missing from exchangeInfo: " + symbol);
    }
};

} // namespace sentum::execution
