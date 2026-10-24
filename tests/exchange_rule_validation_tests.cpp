#include <sentum/trader/execution/ExchangeMetadataCache.hpp>
#include <sentum/trader/execution/ExchangeRules.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <locale>
#include <mutex>
#include <stdexcept>
#include <string>

namespace {
using nlohmann::json;
using sentum::execution::ExchangeMetadataCache;
using sentum::execution::ExchangeRules;
using namespace std::chrono_literals;
void require(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }
template<class Action> void rejects(Action action, const std::string& message) {
    try { action(); } catch (const std::exception&) { return; }
    throw std::runtime_error(message);
}
json lot(const std::string& type, const std::string& minimum, const std::string& maximum, const std::string& step) {
    return {{"filterType", type}, {"minQty", minimum}, {"maxQty", maximum}, {"stepSize", step}};
}
json metadata(const json& filters) {
    return {{"symbols", json::array({{
        {"symbol", "BTCUSDT"}, {"status", "TRADING"}, {"isSpotTradingAllowed", true},
        {"baseAsset", "BTC"}, {"quoteAsset", "USDT"}, {"baseAssetPrecision", 8}, {"quoteAssetPrecision", 8},
        {"orderTypes", json::array({"MARKET", "LIMIT"})}, {"permissionSets", json::array({json::array({"SPOT"})})},
        {"filters", filters}}})}};
}
json baseline() { return metadata(json::array({lot("LOT_SIZE", "0.00001", "100", "0.00001")})); }
ExchangeRules rules(const json& input) { return ExchangeRules::from_exchange_info(input, "BTCUSDT"); }

void lot_intersections() {
    const auto ordinary = lot("LOT_SIZE", "0.10", "10", "0.02");
    for (int mask = 0; mask != 8; ++mask) {
        const auto market = lot("MARKET_LOT_SIZE", mask & 1 ? "0.12" : "0", mask & 2 ? "5" : "0", mask & 4 ? "0.03" : "0");
        for (bool reverse : {false, true}) {
            const auto parsed = rules(metadata(reverse ? json::array({market, ordinary}) : json::array({ordinary, market})));
            require(parsed.min_quantity == ((mask & 1) ? 0.12 : 0.10), "disabled market minimum erased LOT_SIZE");
            require(parsed.max_quantity == ((mask & 2) ? 5.0 : 10.0), "disabled market maximum erased LOT_SIZE");
            require(parsed.step_size == ((mask & 4) ? 0.06 : 0.02), "combined quantity step is not the exact common grid");
            const auto quantity = parsed.normalize_market_quantity(0.29);
            require(quantity.text == ((mask & 4) ? "0.24" : "0.28"), "normalization does not satisfy both lot grids");
            parsed.validate_market_quantity(quantity.text);
            rejects([&] { parsed.normalize_quantity(0.09); }, "quantity below minimum accepted");
            rejects([&] { parsed.normalize_quantity(11.0); }, "quantity above maximum accepted");
        }
    }
    rejects([&] { rules(metadata(json::array({ordinary, lot("MARKET_LOT_SIZE", "11", "0", "0")}))); },
            "contradictory min/max accepted");
    rejects([&] { rules(metadata(json::array({lot("LOT_SIZE", "0.11", "0.12", "0.10")}))); },
            "empty set of permitted quantity increments accepted");
    auto input = baseline();
    input["symbols"][0]["filters"].push_back(lot("MARKET_LOT_SIZE", "0", "0", "0"));
    const auto parsed = rules(input);
    require(parsed.step_size == 0.00001, "zero MARKET_LOT_SIZE step erased valid LOT_SIZE");
}

void exact_quantities() {
    auto parsed = rules(metadata(json::array({lot("LOT_SIZE", "0", "0", "0.1")})));
    require(parsed.normalize_market_quantity(0.3).text == "0.3", "exact decimal lot was rounded down spuriously");
    require(parsed.normalize_market_quantity(std::nextafter(0.3, 0.0)).text == "0.2", "normalization rounded approved size upward");
    require(parsed.normalize_market_quantity(0.39, 0.06).text == "0.3", "local lot step did not intersect the exchange grid");
    rejects([&] { parsed.normalize_market_quantity(0.3, std::numeric_limits<double>::quiet_NaN()); },
            "non-finite local lot step accepted");
    parsed = rules(metadata(json::array({lot("LOT_SIZE", "0", "0", "0.00000001")})));
    require(parsed.normalize_market_quantity(0.000000015).text == "0.00000001", "small quantity requires fixed decimal serialization");
    require(parsed.normalize_market_quantity(0.123456789).text == "0.12345678", "quantity was not truncated to exchange precision");
    for (double bad : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
                       -std::numeric_limits<double>::infinity()})
        rejects([&] { parsed.normalize_quantity(bad); }, "invalid quantity accepted");
    for (const auto& bad : {"nan", "inf", "1e-8", "0.000000001", "1,23", "1junk", "-1", ".1", "1."})
        rejects([&] { parsed.validate_market_quantity(bad); }, "malformed exact quantity accepted");
    class DecimalComma : public std::numpunct<char> { char do_decimal_point() const override { return ','; } };
    const std::locale original;
    std::locale::global(std::locale(original, new DecimalComma));
    try {
        require(parsed.normalize_market_quantity(0.123456789).text == "0.12345678", "locale changed signed quantity formatting");
    } catch (...) { std::locale::global(original); throw; }
    std::locale::global(original);
}

void notional_filters() {
    const json minimum{{"filterType", "MIN_NOTIONAL"}, {"minNotional", "10"}, {"applyToMarket", true}, {"avgPriceMins", 5}};
    const json notional{{"filterType", "NOTIONAL"}, {"minNotional", "20"}, {"maxNotional", "100"},
                        {"applyMinToMarket", false}, {"applyMaxToMarket", true}, {"avgPriceMins", 0}};
    for (bool reverse : {false, true}) {
        auto filters = reverse ? json::array({notional, minimum}) : json::array({minimum, notional});
        filters.push_back(lot("LOT_SIZE", "0", "0", "0.1"));
        const auto parsed = rules(metadata(filters));
        require(parsed.notional_price_windows() == std::set<int>({0, 5}), "notional windows were collapsed");
        parsed.validate_market_notional(1.0, {{5, 10.0}, {0, 100.0}});
        rejects([&] { parsed.validate_market_notional(1.0, {{5, 9.99}, {0, 80.0}}); }, "average minimum notional ignored");
        rejects([&] { parsed.validate_market_notional(1.0, {{5, 30.0}, {0, 100.01}}); }, "market maximum notional ignored");
        rejects([&] { parsed.validate_market_notional(1.0, {{0, 50.0}}); }, "last price substituted for missing average window");
        parsed.validate_market_notional(1.0, {{5, 1.0}, {0, 1000.0}}, 15.0);
        rejects([&] { parsed.validate_market_notional(1.0, {{5, 50.0}, {0, 50.0}}, 9.0); }, "exchange reference price did not take precedence");
        rejects([&] { parsed.validate_market_notional(std::numeric_limits<double>::max(), {}, 2.0); }, "overflowed notional accepted");
        rejects([&] { parsed.validate_market_notional(1.0, {{5, std::numeric_limits<double>::quiet_NaN()}, {0, 50}}); }, "NaN price accepted");
    }
    auto disabled = notional;
    disabled["applyMaxToMarket"] = false;
    const auto parsed = rules(metadata(json::array({disabled})));
    require(parsed.notional_price_windows().empty(), "disabled notional rules demand market prices");
    parsed.validate_market_notional(0.1, {});
    auto exact = minimum;
    exact["minNotional"] = "0.03";
    exact["avgPriceMins"] = 0;
    rules(metadata(json::array({exact}))).validate_market_notional(0.1, {{0, 0.3}});
    auto contradictory = notional;
    contradictory["maxNotional"] = "5";
    contradictory["minNotional"] = "0";
    contradictory["avgPriceMins"] = 5;
    rejects([&] { rules(metadata(json::array({minimum, contradictory}))); }, "contradictory enabled notional bounds accepted");
}

void metadata_schema_and_permissions() {
    const auto original = baseline();
    for (const std::string key : {"status", "isSpotTradingAllowed", "orderTypes", "baseAsset", "quoteAsset", "baseAssetPrecision", "quoteAssetPrecision", "permissionSets", "filters"}) {
        auto missing = original;
        missing["symbols"][0].erase(key);
        rejects([&] { rules(missing); }, "missing required exchange field accepted: " + key);
    }
    for (const auto& bad : {json("nan"), json("Infinity"), json("0.01junk"), json("-0.01"), json(0.01), json(nullptr)}) {
        auto input = original;
        input["symbols"][0]["filters"][0]["stepSize"] = bad;
        rejects([&] { rules(input); }, "invalid exchange numeric token accepted");
    }
    auto input = original;
    input["symbols"][0]["status"] = "HALT";
    rejects([&] { rules(input); }, "halted symbol accepted");
    input = original; input["symbols"][0]["isSpotTradingAllowed"] = false;
    rejects([&] { rules(input); }, "non-Spot symbol accepted");
    input = original; input["symbols"][0]["orderTypes"] = json::array({"LIMIT"});
    rejects([&] { rules(input); }, "symbol without MARKET order support accepted");
    input = original; input["symbols"][0]["baseAssetPrecision"] = std::numeric_limits<std::uint64_t>::max();
    rejects([&] { rules(input); }, "overflowing exchange precision accepted");
    input = original; input["symbols"].push_back(input["symbols"][0]);
    rejects([&] { rules(input); }, "duplicate exchange symbol accepted");
    input = original; input["symbols"][0]["filters"].push_back(input["symbols"][0]["filters"][0]);
    rejects([&] { rules(input); }, "duplicate exchange filter accepted");
    input = original; input["symbols"][0]["symbol"] = "ETHUSDT";
    rejects([&] { rules(input); }, "wrong-symbol metadata accepted");
    input = original; input["symbols"][0]["permissionSets"] = json::array({json::array({"SPOT", "MARGIN"}), json::array({"TRD_GRP_004", "TRD_GRP_005"})});
    const auto parsed = rules(input);
    parsed.validate_permissions({"SPOT", "TRD_GRP_005"});
    rejects([&] { parsed.validate_permissions({"SPOT"}); }, "permission groups were treated as optional");
    rejects([&] { parsed.validate_permissions({"TRD_GRP_004"}); }, "base Spot permission was ignored");
}

class MetadataExchange final : public BinanceSpotExecutionClient {
public:
    MetadataExchange() : BinanceSpotExecutionClient("fixture", "fixture") {}
    json exchange_info(const std::string&) const override {
        ++calls;
        json result;
        std::function<void()> callback;
        { std::lock_guard<std::mutex> lock(mutex); result = response; callback = after_capture; }
        if (callback) callback();
        return result;
    }
    void configure(json value, std::function<void()> callback = {}) {
        std::lock_guard<std::mutex> lock(mutex); response = std::move(value); after_capture = std::move(callback);
    }
    mutable std::atomic<int> calls{0};
private:
    mutable std::mutex mutex;
    json response = baseline();
    std::function<void()> after_capture;
};

void cache_refresh_and_invalidation() {
    MetadataExchange exchange;
    ExchangeMetadataCache cache(exchange);
    require(cache.rules("BTCUSDT").step_size == 0.00001 && exchange.calls == 1, "metadata was not loaded");
    (void)cache.rules("BTCUSDT");
    require(exchange.calls == 1, "unexpired metadata cache was ignored");
    auto changed = baseline(); changed["symbols"][0]["filters"][0]["stepSize"] = "0.005";
    exchange.configure(changed);
    require(cache.rules("BTCUSDT", true).step_size == 0.005, "forced metadata refresh retained old constraints");
    exchange.configure(json::object());
    rejects([&] { cache.rules("BTCUSDT", true); }, "failed refresh fell back to stale rules");
    rejects([&] { cache.rules("BTCUSDT"); }, "invalidated metadata became usable after failure");
    std::promise<void> captured, release;
    auto release_future = release.get_future().share();
    exchange.configure(baseline(), [&] { captured.set_value(); release_future.wait(); });
    auto pending = std::async(std::launch::async, [&] {
        try { cache.rules("BTCUSDT", true); return false; } catch (const std::exception&) { return true; }
    });
    const bool entered = captured.get_future().wait_for(2s) == std::future_status::ready;
    if (!entered) { release.set_value(); (void)pending.get(); throw std::runtime_error("metadata fixture did not enter request"); }
    cache.invalidate("BTCUSDT");
    exchange.configure(changed);
    release.set_value();
    require(pending.get(), "invalidated in-flight metadata request was accepted");
    require(cache.rules("BTCUSDT").step_size == 0.005, "old in-flight metadata overwrote invalidation");

    cache.clear();
    std::promise<void> old_captured, old_release;
    auto old_release_future = old_release.get_future().share();
    exchange.configure(baseline(), [&] { old_captured.set_value(); old_release_future.wait(); });
    auto old_request = std::async(std::launch::async, [&] {
        try { cache.rules("BTCUSDT", true); return false; } catch (const std::exception&) { return true; }
    });
    const bool old_entered = old_captured.get_future().wait_for(2s) == std::future_status::ready;
    if (!old_entered) { old_release.set_value(); (void)old_request.get(); throw std::runtime_error("old metadata request did not start"); }
    exchange.configure(changed);
    const auto newer = cache.rules("BTCUSDT");
    old_release.set_value();
    require(newer.step_size == 0.005 && old_request.get(), "out-of-order metadata requests accepted older constraints");
    require(cache.rules("BTCUSDT").step_size == 0.005, "old response overwrote newer cached metadata");
}
} // namespace

int main() {
    try {
        lot_intersections(); exact_quantities(); notional_filters(); metadata_schema_and_permissions(); cache_refresh_and_invalidation();
        std::cout << "exchange rule validation tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "exchange rule validation tests failed: " << error.what() << '\n';
        return 1;
    }
}
