#include <sentum/collector/FastBinanceKlineParser.hpp>
#include <iostream>
#include <string>

int main() {
    using sentum::collector::FastBinanceKlineParser;
    using sentum::collector::ParsedKline;
    const std::string prefix = R"({"E":1710000000100,"k":{"s":"BTCUSDT","t":)";
    const std::string suffix = R"(,"o":"1.2","h":"2","l":"1","c":"1.5","v":"3","x":true}})";
    ParsedKline value{};
    if (!FastBinanceKlineParser::parse(prefix + "1710000000000" + suffix, value)) return 1;
    if (value.timestamp != 1710000000000LL || value.close != 1.5) return 2;
    if (FastBinanceKlineParser::parse(prefix + "9223372036854775808" + suffix, value)) return 3;
    if (FastBinanceKlineParser::parse(prefix + "-9223372036854775809" + suffix, value)) return 4;
    if (FastBinanceKlineParser::parse(prefix + "1710000000000" + R"(,"o":"1.2","h":"2","l":"1","c":"123garbage","v":"3","x":true}})", value)) return 5;
    if (FastBinanceKlineParser::parse(prefix + "1710000000000" + R"(,"o":"1.2","h":"2","l":"1","c":"1e9999","v":"3","x":true}})", value)) return 6;
    std::cout << "kline numeric validation passed\n";
    return 0;
}
