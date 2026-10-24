#include <sentum/backtest/Backtest.hpp>
#include <sentum/core/RuntimeControl.hpp>
#include <sentum/time/Clock.hpp>
#include <sentum/trader/TradeEngine.hpp>
#include <sentum/trader/execution/SimulatedExecutionVenue.hpp>
#include <sentum/trader/order/OrderManager.hpp>
#include <sentum/trader/risk/RiskManager.hpp>
#include <sentum/trader/utils/RiskConfigLoader.hpp>
#include <sentum/trader/utils/StrictNumeric.hpp>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>

namespace {
using namespace std::chrono_literals;
using Time = std::chrono::system_clock::time_point;
using sentum::order::Request;
using sentum::order::Side;
void require(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }
template<class Action> void rejects(Action action, const std::string& message) {
    try { action(); } catch (const std::exception&) { return; }
    throw std::runtime_error(message);
}
const std::vector<double> nonfinite{std::numeric_limits<double>::quiet_NaN(),
    std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()};
const std::vector<double> invalid_positive{0.0, -1.0, std::numeric_limits<double>::quiet_NaN(),
    std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()};

class Workspace {
public:
    Workspace() : previous_(std::filesystem::current_path()) {
        std::string pattern = (std::filesystem::temp_directory_path() / "sentum-numeric-validation-XXXXXX").string();
        const auto created = ::mkdtemp(pattern.data());
        if (!created) throw std::runtime_error("Cannot create numeric validation workspace");
        path_ = created;
        std::filesystem::current_path(path_);
        std::filesystem::create_directory("log");
    }
    ~Workspace() {
        std::error_code ignored;
        std::filesystem::current_path(previous_, ignored);
        std::filesystem::remove_all(path_, ignored);
    }
private:
    std::filesystem::path previous_, path_;
};

nlohmann::json risk_json() {
    return {{"max_total_capital", 1000.0}, {"risk_per_trade", 0.01}, {"stop_loss_percent", 0.02},
        {"take_profit_percent", 0.04}, {"buy_fee_percent", 0.001}, {"sell_fee_percent", 0.001},
        {"slippage_percent", 0.0005}, {"spread_percent", 0.0002}, {"leverage", 1.0},
        {"min_quantity", 0.000001}, {"max_quantity", 0.0}, {"step_size", 0.000001},
        {"min_notional", 5.0}, {"cooldown_seconds", 30}, {"max_holding_seconds", 900}, {"max_data_age_ms", 2000}};
}

void configuration_boundaries() {
    const std::vector<double RiskConfig::*> members{&RiskConfig::max_total_capital, &RiskConfig::risk_per_trade,
        &RiskConfig::stop_loss_percent, &RiskConfig::take_profit_percent, &RiskConfig::trailing_sl_percent,
        &RiskConfig::trailing_tp_percent, &RiskConfig::buy_fee_percent, &RiskConfig::sell_fee_percent,
        &RiskConfig::slippage_percent, &RiskConfig::spread_percent, &RiskConfig::leverage,
        &RiskConfig::min_quantity, &RiskConfig::max_quantity, &RiskConfig::step_size, &RiskConfig::min_notional};
    for (const auto member : members) for (const double bad : nonfinite) {
        RiskConfig config;
        config.*member = bad;
        rejects([&] { RiskManager risk(config); }, "non-finite directly constructed risk configuration accepted");
    }
    for (const std::string field : {"max_total_capital", "risk_per_trade", "stop_loss_percent", "take_profit_percent",
          "buy_fee_percent", "sell_fee_percent", "slippage_percent", "spread_percent", "leverage",
          "min_quantity", "max_quantity", "step_size", "min_notional", "trailing_sl_percent", "trailing_tp_percent"}) {
        auto input = risk_json();
        input[field] = std::numeric_limits<double>::infinity();
        rejects([&] { parse_risk_config(input); }, "non-finite parsed risk key accepted: " + field);
    }
    for (const auto member : {&RiskConfig::buy_fee_percent, &RiskConfig::sell_fee_percent, &RiskConfig::slippage_percent,
                              &RiskConfig::trailing_tp_percent}) {
        RiskConfig config; config.*member = 1.0;
        rejects([&] { RiskManager risk(config); }, "cost permitting zero or negative proceeds accepted");
    }
    RiskConfig config;
    config.spread_percent = 2.0;
    rejects([&] { RiskManager risk(config); }, "spread permitting zero sell price accepted");
    config = RiskConfig{}; config.max_quantity = -1.0;
    rejects([&] { RiskManager risk(config); }, "negative maximum quantity accepted");
    config = RiskConfig{}; config.step_size = 0.0;
    rejects([&] { RiskManager risk(config); }, "zero quantity step accepted");
    config = RiskConfig{}; config.max_total_capital = std::numeric_limits<double>::max(); config.leverage = 2.0;
    rejects([&] { RiskManager risk(config); }, "overflowed capital cap accepted");
    config = RiskConfig{}; config.max_total_capital = std::numeric_limits<double>::denorm_min(); config.risk_per_trade = 0.5;
    rejects([&] { RiskManager risk(config); }, "underflowed risk budget accepted");
    config = RiskConfig{}; config.max_holding_seconds = std::numeric_limits<std::int64_t>::max();
    rejects([&] { RiskManager risk(config); }, "overflowing clock duration accepted");
    for (const char* key : {"cooldown_seconds", "max_holding_seconds", "max_data_age_ms"}) {
        auto input = risk_json(); input[key] = std::numeric_limits<std::uint64_t>::max();
        rejects([&] { parse_risk_config(input); }, "unsigned time overflow accepted");
    }
    config = RiskConfig{}; config.take_profit_percent = 10.0;
    RiskManager large_valid_target(config);
    (void)large_valid_target;
}

void risk_price_and_output_boundaries() {
    const Time now{1600000000s};
    const auto clock = std::make_shared<ReplayClock>(now);
    RiskManager manager(RiskConfig{}, clock);
    const StrategySignal signal{TradeAction::BUY, "fixture", "entry", 100.0, now, 1.0};
    for (const double price : invalid_positive)
        require(!manager.approve_entry(signal, price, now, {}).approved, "risk approved invalid price");
    for (const double price : {std::numeric_limits<double>::denorm_min(), std::numeric_limits<double>::max()})
        require(!manager.approve_entry(signal, price, now, {}).approved, "risk approved nonrepresentable position");
    const auto approved = manager.approve_entry(signal, 100.0, now, {});
    require(approved.approved && approved.quantity == 5.0 && approved.notional == 500.0,
            "valid finite risk approval changed");
    require(!manager.approve_entry(signal, 100.0, Time::min(), {}).approved,
            "overflowed historical clock difference made stale data fresh");
    require(!manager.approve_entry(signal, 100.0, now, Time::max()).approved,
            "overflowed exit clock difference bypassed cooldown");
    RiskConfig tiny_step;
    tiny_step.step_size = std::numeric_limits<double>::denorm_min();
    RiskManager tiny(tiny_step, clock);
    require(!tiny.approve_entry(signal, 100.0, now, {}).approved, "overflowed quantity grid approved");
    RiskConfig extreme_target;
    extreme_target.take_profit_percent = std::numeric_limits<double>::max();
    RiskManager target(extreme_target, clock);
    require(!target.approve_entry(signal, 100.0, now, {}).approved, "overflowed target approved before dispatch");
    RiskConfig extreme_fill;
    extreme_fill.max_total_capital = 1.0e308;
    extreme_fill.step_size = 0.1;
    extreme_fill.spread_percent = 1.5;
    extreme_fill.slippage_percent = 0.9;
    RiskManager fill(extreme_fill, clock);
    require(!fill.approve_entry(signal, 1.0e308, now, {}).approved, "cost-adjusted fill overflow approved");
    extreme_target = RiskConfig{};
    extreme_target.max_total_capital = 1.0e307;
    extreme_target.take_profit_percent = 1000.0;
    extreme_target.step_size = 1.0;
    RiskManager target_notional(extreme_target, clock);
    require(!target_notional.approve_entry(signal, 1.0e305, now, {}).approved,
            "finite target price with overflowing exit notional approved");
}

void csv_boundaries() {
    const std::vector<std::string> rows{
        "1600000000000,nan", "1600000000000,inf", "1600000000000,-inf", "1600000000000,1e309",
        "1600000000000,1e-500", "1600000000000,1tail", "1600000000000,0", "1600000000000,-1",
        "1600000000000, 1", "1600000000000,0x1p4", "1600000000000,", "1600000000000,1,",
        "1600000000000,1,nan", "1600000000000,1,inf", "1600000000000,1,-1", "1600000000000,1,1tail",
        "1600000000000,1,0,extra", "1600000000000junk,1", "1.5,1", "nan,1", ",1",
        "9223372036854775807,1", "18446744073709551615,1", "-9223372036854775808,1"};
    for (const auto& row : rows) {
        { std::ofstream file("malformed.csv"); file << "timestamp_ms,price,volume\n" << row << '\n'; }
        bool rejected = false;
        try { (void)HistoricalEventReader::read_csv("malformed.csv", "BTCUSDT"); }
        catch (const std::exception& error) {
            rejected = true;
            require(std::string(error.what()).find("malformed.csv:2:") != std::string::npos,
                    "CSV error omitted file and line diagnostics");
        }
        require(rejected, "malformed CSV number accepted: " + row);
    }
    { std::ofstream file("valid.csv"); file << "timestamp_ms,price,volume\r\n1600000001000,1e2,0\r\n1600000000000,99.9,2.5\r\n"; }
    const auto events = HistoricalEventReader::read_csv("valid.csv", "BTCUSDT");
    require(events.size() == 2 && events[0].price == 99.9 && events[1].price == 100.0 && events[1].volume == 0.0,
            "valid CSV parsing and time ordering changed");
}

void execution_boundaries() {
    using sentum::execution::SimulatedExecutionVenue;
    SimulatedExecutionVenue venue;
    int callbacks = 0;
    venue.start([&](const auto&) { ++callbacks; });
    const Time now{1600000000s};
    venue.set_market(100.0, now);
    for (double bad : invalid_positive) {
        rejects([&] { venue.set_market(bad, now); }, "invalid simulated market price accepted");
        rejects([&] { venue.submit(Request{"BTCUSDT", Side::Buy, bad, "bad"}); }, "invalid simulated quantity accepted");
    }
    for (double bad : nonfinite) {
        rejects([&] { venue.set_fill_model(bad, 0.0); }, "non-finite spread accepted");
        rejects([&] { venue.set_fill_model(0.0, bad); }, "non-finite slippage accepted");
    }
    rejects([&] { venue.set_fill_model(2.0, 0.0); }, "zero sell-touch model accepted");
    rejects([&] { venue.set_fill_model(0.0, 1.0); }, "zero sell-fill model accepted");
    rejects([&] { venue.submit(Request{"BTCUSDT", Side::Buy, std::numeric_limits<double>::max(), "overflow"}); }, "overflowed simulated notional accepted");
    require(callbacks == 0, "rejected simulation published execution evidence");
    const auto valid = venue.submit(Request{"BTCUSDT", Side::Buy, 1.0, "valid"});
    require(callbacks == 1 && valid.average_fill_price == 100.0 && valid.cumulative_quote_quantity == 100.0,
            "invalid simulated setter replaced prior valid price or fill model");
    venue.set_fill_model(0.5, 0.5);
    venue.set_market(std::numeric_limits<double>::max(), now);
    rejects([&] { venue.submit(Request{"BTCUSDT", Side::Buy, 1.0, "overflow-fill"}); }, "overflowed simulated fill price accepted");
    BinanceSpotExecutionClient client("fixture", "fixture");
    for (double bad : invalid_positive)
        rejects([&] { client.place_market_order(Request{"BTCUSDT", Side::Buy, bad, "invalid", {}, {}, "1"}); },
                "real execution boundary accepted invalid quantity before transport");
}

class CountingExchange final : public BinanceSpotExecutionClient {
public:
    CountingExchange() : BinanceSpotExecutionClient("fixture", "fixture") {}
    nlohmann::json open_orders() const override { return nlohmann::json::array(); }
    nlohmann::json place_market_order(const Request&) const override { ++placements; throw std::runtime_error("unexpected placement"); }
    mutable int placements = 0;
};
void order_admission_boundaries() {
    CountingExchange client;
    sentum::order::OrderManager manager(client);
    manager.reconcile_startup();
    int callbacks = 0;
    manager.set_update_handler([&](const auto&) { ++callbacks; });
    for (double bad : invalid_positive)
        rejects([&] { manager.submit_market(Request{"BTCUSDT", Side::Buy, bad, "invalid"}); }, "order manager accepted invalid quantity");
    require(callbacks == 0 && client.placements == 0, "invalid order crossed intent or transport boundary");
}

class BuyStrategy final : public IStrategy {
public:
    StrategySignal on_price(double price, Time now) override {
        ++observations;
        return {TradeAction::BUY, "fixture", "entry", price, now, 1.0};
    }
    void reset() override {}
    int observations = 0;
};
void trade_engine_boundaries() {
    sentum::runtime::RuntimeControl::global().pause_entries(false);
    (void)sentum::runtime::RuntimeControl::global().consume_manual_close();
    const Time now{1600000000s};
    auto clock = std::make_shared<ReplayClock>(now);
    auto strategy = std::make_unique<BuyStrategy>();
    auto* observer = strategy.get();
    TradeEngine engine("BTCUSDT", RiskConfig{}, clock, std::move(strategy));
    for (double bad : invalid_positive) {
        MarketEvent event; event.symbol = "BTCUSDT"; event.timestamp = now; event.price = bad; event.close = bad;
        require(engine.process_event(event) == TradeAction::NONE && engine.evaluate(bad) == TradeAction::NONE,
                "invalid market price reached trade evaluation");
    }
    require(observer->observations == 0 && !engine.get_current_position().open && engine.get_latest_price() == 0.0,
            "invalid input mutated strategy, position or last-price state");
    require(engine.evaluate(100.0) == TradeAction::BUY, "valid finite engine input did not open a position");
    const auto position = engine.get_current_position();
    sentum::runtime::RuntimeControl::global().request_manual_close();
    for (double bad : invalid_positive)
        require(engine.evaluate(bad) == TradeAction::NONE, "invalid price closed a simulated position");
    require(engine.get_current_position().quantity == position.quantity && engine.completed_trades().empty(),
            "invalid exit input changed held inventory");
    require(engine.evaluate(101.0) == TradeAction::SELL && engine.completed_trades().size() == 1,
            "invalid exit input consumed the pending close request");
    require(std::isfinite(engine.completed_trades().front().net_profit), "completed trade retained invalid arithmetic");
}
} // namespace

int main() {
    try {
        Workspace workspace;
        configuration_boundaries(); risk_price_and_output_boundaries(); csv_boundaries(); execution_boundaries();
        order_admission_boundaries(); trade_engine_boundaries();
        std::cout << "numeric boundary tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "numeric boundary tests failed: " << error.what() << '\n';
        return 1;
    }
}
