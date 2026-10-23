#include <sentum/time/Clock.hpp>
#include <sentum/trader/TradeEngine.hpp>
#include <sentum/trader/risk/RiskManager.hpp>
#include <sentum/trader/strategy/MomentumStrategy.hpp>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace std::chrono_literals;
using TimePoint = std::chrono::system_clock::time_point;
constexpr auto one_tick = std::chrono::system_clock::duration{1};

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void require_near(double actual, double expected, const std::string& message) {
    require(std::abs(actual - expected) < 1e-9, message);
}

RiskConfig test_risk() {
    RiskConfig risk;
    risk.max_total_capital = 1000.0;
    risk.risk_per_trade = 0.01;
    risk.stop_loss_percent = 0.02;
    risk.take_profit_percent = 0.04;
    risk.step_size = 0.01;
    risk.buy_fee_percent = 0.0;
    risk.sell_fee_percent = 0.0;
    risk.slippage_percent = 0.0;
    risk.spread_percent = 0.0;
    risk.cooldown_seconds = 30;
    risk.max_data_age_ms = 2000;
    return risk;
}

StrategySignal buy_signal(TimePoint timestamp) {
    return {TradeAction::BUY, "momentum", "test entry", 100.0, timestamp, 1.0};
}

MarketEvent event_at(TimePoint timestamp, double price) {
    MarketEvent event;
    event.symbol = "BTCUSDT";
    event.timestamp = timestamp;
    event.price = price;
    event.close = price;
    return event;
}

void test_risk_freshness_boundaries() {
    const auto now = TimePoint{946684800s};
    auto clock = std::make_shared<ReplayClock>(now);
    RiskManager risk(test_risk(), clock);

    const auto at_limit = now - 2000ms;
    require(risk.approve_entry(buy_signal(at_limit), 100.0, at_limit, {}).approved,
            "data exactly at the configured maximum age must remain fresh");
    const auto too_old = at_limit - one_tick;
    const auto stale = risk.approve_entry(buy_signal(too_old), 100.0, too_old, {});
    require(!stale.approved && stale.reason == "market data is stale",
            "data one clock tick beyond the configured maximum age must be rejected");

    clock->advance_to(now + one_tick);
    require(!risk.approve_entry(buy_signal(at_limit), 100.0, at_limit, {}).approved,
            "risk checks must observe subsequent advances of their shared clock");
}

void test_risk_cooldown_boundaries() {
    const auto last_exit = TimePoint{946684800s};
    auto clock = std::make_shared<ReplayClock>(last_exit + 30s - one_tick);
    RiskManager risk(test_risk(), clock);
    auto price_time = clock->now();
    const auto blocked = risk.approve_entry(buy_signal(price_time), 100.0, price_time, last_exit);
    require(!blocked.approved && blocked.reason == "cooldown active",
            "entry one clock tick before cooldown expiry must be rejected");

    clock->advance_to(last_exit + 30s);
    // A fresh but delayed market event must not replace the clock's evaluation time.
    price_time = clock->now() - 2s;
    require(risk.approve_entry(buy_signal(price_time), 100.0, price_time, last_exit).approved,
            "cooldown must expire at the evaluation clock's exact boundary");
}

void test_system_clock_default() {
    RiskManager risk(test_risk());
    const auto historical = TimePoint{946684800s};
    const auto stale = risk.approve_entry(buy_signal(historical), 100.0, historical, {});
    require(!stale.approved && stale.reason == "market data is stale",
            "the default live risk clock must reject historical data");

    const auto recent = std::chrono::system_clock::now();
    require(risk.approve_entry(buy_signal(recent), 100.0, recent, {}).approved,
            "the default live risk clock must accept current data");

    bool rejected_null = false;
    try {
        RiskManager invalid(test_risk(), nullptr);
    } catch (const std::invalid_argument&) {
        rejected_null = true;
    }
    require(rejected_null, "a missing explicitly supplied clock must be rejected");
}

void test_engine_freshness_boundaries() {
    const auto base = TimePoint{946684800s};
    for (const auto delay : {std::chrono::system_clock::duration{2000ms},
                             std::chrono::system_clock::duration{2000ms} + one_tick}) {
        auto clock = std::make_shared<ReplayClock>(base);
        TradeEngine engine("BTCUSDT", test_risk(), clock, std::make_unique<MomentumStrategy>(2, 0.001));
        require(engine.process_event(event_at(base, 100.0)) == TradeAction::NONE,
                "momentum must warm up before entry");
        const auto observed_at = base + 1s;
        clock->advance_to(observed_at + delay);
        const auto action = engine.process_event(event_at(observed_at, 101.0));
        const bool at_limit = delay == std::chrono::system_clock::duration{2000ms};
        require(action == (at_limit ? TradeAction::BUY : TradeAction::NONE),
                "TradeEngine must preserve the exact configured event freshness boundary");
        require(engine.get_current_position().open == at_limit,
                "stale market data must not create a simulated fill");
    }
}

std::vector<TradePosition> run_historical_sequence(TimePoint base) {
    auto clock = std::make_shared<ReplayClock>(base);
    TradeEngine engine("BTCUSDT", test_risk(), clock, std::make_unique<MomentumStrategy>(2, 0.001));
    auto process = [&](std::chrono::system_clock::duration elapsed, double price) {
        clock->advance_to(base + elapsed);
        return engine.process_event(event_at(base + elapsed, price));
    };

    require(process(0s, 100.0) == TradeAction::NONE, "historical strategy warmup failed");
    require(process(1s, 101.0) == TradeAction::BUY, "historical momentum must create its first fill");
    require_near(engine.get_current_position().quantity, 4.95, "first fill has incorrect risk sizing");
    require(process(2s, 106.0) == TradeAction::SELL, "historical take profit must close the first fill");
    require(process(3s, 100.0) == TradeAction::NONE, "strategy must warm up again after exit");
    require(process(32s - one_tick, 101.0) == TradeAction::NONE,
            "historical TradeEngine must enforce the cooldown until its exact boundary");
    require(!engine.get_current_position().open, "cooldown rejection must leave the engine flat");
    require(process(32s, 102.0) == TradeAction::BUY,
            "historical TradeEngine must allow a fill exactly when cooldown expires");
    require(process(33s, 107.0) == TradeAction::SELL, "historical second take profit must close");

    const auto trades = engine.completed_trades();
    require(trades.size() == 2 && engine.get_total_trades() == 2,
            "historical replay must produce exactly two completed trades, not an empty result");
    require(trades[0].entry_time == base + 1s && trades[0].exit_time == base + 2s,
            "first fill must retain the historical event timestamps");
    require(trades[1].entry_time == base + 32s && trades[1].exit_time == base + 33s,
            "second fill must retain the historical event timestamps");
    require_near(trades[0].entry_price, 101.0, "first historical entry price changed");
    require_near(trades[0].exit_price, 106.0, "first historical exit price changed");
    require_near(trades[0].quantity, 4.95, "first historical fill quantity changed");
    require_near(trades[0].net_profit, 24.75, "first historical realized profit changed");
    require_near(trades[1].entry_price, 102.0, "second historical entry price changed");
    require_near(trades[1].exit_price, 107.0, "second historical exit price changed");
    require_near(trades[1].quantity, 4.90, "second historical fill quantity changed");
    require_near(trades[1].net_profit, 24.50, "second historical realized profit changed");
    require_near(engine.get_total_profit(), 49.25, "historical replay profit must match known fills");
    return trades;
}

void test_historical_fills_are_independent_of_calendar_date() {
    // Identical relative events in 2001, 2024 and 2040 must execute identically.
    const std::vector<TimePoint> dates{TimePoint{978307200s}, TimePoint{1710000000s}, TimePoint{2208988800s}};
    const auto reference = run_historical_sequence(dates.front());
    for (std::size_t i = 1; i < dates.size(); ++i) {
        const auto shifted = run_historical_sequence(dates[i]);
        for (std::size_t trade = 0; trade < reference.size(); ++trade) {
            require_near(shifted[trade].quantity, reference[trade].quantity,
                         "calendar shifts must not change fill quantity");
            require_near(shifted[trade].net_profit, reference[trade].net_profit,
                         "calendar shifts must not change realized profit");
            require(shifted[trade].entry_time - dates[i] == reference[trade].entry_time - dates.front(),
                    "calendar shifts must preserve relative entry time");
            require(shifted[trade].exit_time - dates[i] == reference[trade].exit_time - dates.front(),
                    "calendar shifts must preserve relative exit time");
        }
    }
}
} // namespace

int main() {
    try {
        std::filesystem::create_directories("log");
        test_risk_freshness_boundaries();
        test_risk_cooldown_boundaries();
        test_system_clock_default();
        test_engine_freshness_boundaries();
        test_historical_fills_are_independent_of_calendar_date();
        std::cout << "replay risk clock tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "replay risk clock tests failed: " << error.what() << '\n';
        return 1;
    }
}
