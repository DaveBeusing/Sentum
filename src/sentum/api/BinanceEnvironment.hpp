#pragma once

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>

namespace sentum::api {

enum class BinanceEnvironment { PublicMarketData, SpotTestnet };

inline const char* environment_name(BinanceEnvironment environment) noexcept {
    switch (environment) {
        case BinanceEnvironment::SpotTestnet: return "binance-spot-testnet";
        case BinanceEnvironment::PublicMarketData: return "binance-public-market-data";
    }
    return "invalid";
}

inline const char* market_stream_host(BinanceEnvironment environment) noexcept {
    return environment == BinanceEnvironment::SpotTestnet ? "stream.testnet.binance.vision" : "stream.binance.com";
}

inline std::string market_trade_url(BinanceEnvironment environment, std::string symbol) {
    if (environment != BinanceEnvironment::SpotTestnet && environment != BinanceEnvironment::PublicMarketData)
        throw std::invalid_argument("Unknown Binance market environment");
    if (symbol.empty() || symbol.size() > 32 ||
        !std::all_of(symbol.begin(), symbol.end(), [](unsigned char value) { return std::isalnum(value); }))
        throw std::invalid_argument("Invalid market stream symbol");
    std::transform(symbol.begin(), symbol.end(), symbol.begin(),
        [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    return std::string("wss://") + market_stream_host(environment) +
        (environment == BinanceEnvironment::SpotTestnet ? "/ws/" : ":9443/ws/") + symbol + "@trade";
}

} // namespace sentum::api
