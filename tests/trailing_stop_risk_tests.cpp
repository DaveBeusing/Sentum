#include <sentum/core/RuntimeControl.hpp>
#include <sentum/time/Clock.hpp>
#include <sentum/trader/TradeEngine.hpp>
#include <sentum/trader/risk/RiskManager.hpp>
#include <sentum/trader/utils/RiskConfigLoader.hpp>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace {
using namespace std::chrono_literals;
using TimePoint = std::chrono::system_clock::time_point;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void require_near(double actual, double expected, const std::string& message) {
    require(std::isfinite(actual) && std::abs(actual - expected) < 1e-9, message);
}

template<class Function>
void require_rejected(Function&& operation, const std::string& message) {
    bool rejected = false;
    try { operation(); } catch (const std::exception&) { rejected = true; }
    require(rejected, message);
}

class TemporaryDirectory {
public:
    TemporaryDirectory() : previous_(std::filesystem::current_path()) {
        std::string pattern = (std::filesystem::temp_directory_path() / "sentum-trailing-risk-XXXXXX").string();
        const char* created = ::mkdtemp(pattern.data());
        if (!created) throw std::runtime_error("Cannot create trailing-stop fixture");
        path_ = created;
        std::filesystem::current_path(path_);
        std::filesystem::create_directories("log");
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::current_path(previous_, ignored);
        std::filesystem::remove_all(path_, ignored);
    }
private:
    std::filesystem::path previous_;
    std::filesystem::path path_;
};

class BuyStrategy final : public IStrategy {
public:
    StrategySignal on_price(double price, TimePoint now) override {
        return {TradeAction::BUY, "fixture", "entry", price, now, 1.0};
    }
    void reset() override {}
};

RiskConfig risk_config(double trailing_distance) {
    RiskConfig risk;
    risk.max_total_capital = 1000.0;
    risk.risk_per_trade = 0.005;
    risk.stop_loss_percent = 0.01;
    risk.take_profit_percent = 1.0;
    risk.trailing_sl_enabled = true;
    risk.trailing_sl_percent = trailing_distance;
    risk.buy_fee_percent = 0.0;
    risk.sell_fee_percent = 0.0;
    risk.spread_percent = 0.0;
    risk.slippage_percent = 0.0;
    risk.max_holding_seconds = 3600;
    return risk;
}

class ReplayFixture {
public:
    explicit ReplayFixture(RiskConfig risk)
        : clock(std::make_shared<ReplayClock>(TimePoint{1600000000s})),
          engine("BTCUSDT", risk, clock, std::make_unique<BuyStrategy>()) {}

    TradeAction tick(double price) {
        clock->advance_to(clock->now() + 1s);
        MarketEvent event;
        event.symbol = "BTCUSDT";
        event.timestamp = clock->now();
        event.price = price;
        event.close = price;
        return engine.process_event(event);
    }

    std::shared_ptr<ReplayClock> clock;
    TradeEngine engine;
};

void unchanged_approval(const TradePosition& position, const TradePosition& entry) {
    require(position.open && position.risk_approved, "stop update must retain the approved open position");
    require_near(position.quantity, entry.quantity, "stop updates must not resize an approved position");
    require_near(position.capital_at_risk, entry.capital_at_risk, "stop updates must not change the approved risk budget");
    require_near(position.entry_price, entry.entry_price, "stop updates must preserve the confirmed entry fill");
    require(position.stop_loss_price >= entry.stop_loss_price, "stop update increased the initial downside distance");
}

void test_wider_trailing_distance_preserves_initial_protection() {
    ReplayFixture fixture(risk_config(0.10));
    require(fixture.tick(100.0) == TradeAction::BUY, "wide-trailing fixture must open a position");
    const auto entry = fixture.engine.get_current_position();
    require_near(entry.stop_loss_price, 99.0, "initial stop must use the approved one-percent distance");
    require_near(entry.quantity, 5.0, "entry must use the approved position size");
    for (double price : {101.0, 105.0, 110.0}) {
        require(fixture.tick(price) == TradeAction::NONE, "favorable ticks must retain the position");
        const auto position = fixture.engine.get_current_position();
        require_near(position.stop_loss_price, 99.0, "a wider trailing candidate must not lower the initial stop");
        unchanged_approval(position, entry);
    }
    require(fixture.tick(111.0) == TradeAction::NONE, "higher favorable tick must retain the position");
    require_near(fixture.engine.get_current_position().stop_loss_price, 99.9, "trailing must advance once it exceeds initial protection");
    for (double price : {111.0, 110.5, 110.0, 111.0}) {
        require(fixture.tick(price) == TradeAction::NONE, "equal and lower prices above the stop must retain the position");
        const auto position = fixture.engine.get_current_position();
        require_near(position.stop_loss_price, 99.9, "equal or lower highs must not loosen an advanced stop");
        unchanged_approval(position, entry);
    }
}

void test_narrow_trailing_and_gap_preserve_executable_price() {
    ReplayFixture fixture(risk_config(0.005));
    require(fixture.tick(100.0) == TradeAction::BUY, "narrow-trailing fixture must open a position");
    const auto entry = fixture.engine.get_current_position();
    require(fixture.tick(101.0) == TradeAction::NONE, "favorable tick must retain the position");
    const auto advanced = fixture.engine.get_current_position();
    require_near(advanced.stop_loss_price, 100.495, "narrow trailing stop did not advance correctly");
    unchanged_approval(advanced, entry);
    require(fixture.tick(100.8) == TradeAction::NONE, "a pullback above the stop must remain open");
    require_near(fixture.engine.get_current_position().stop_loss_price, advanced.stop_loss_price,
                 "a pullback must not lower the trailing stop");
    require(fixture.tick(98.0) == TradeAction::SELL, "a gap through the stop must close at the observed price");
    require(fixture.engine.completed_trades().size() == 1, "stop exit must record exactly one completed trade");
    const auto closed = fixture.engine.completed_trades().front();
    require(!closed.open && closed.stop_loss_triggered && closed.close_reason == "stop_loss",
            "gap close must retain a completed stop-loss record");
    require_near(closed.exit_price, 98.0, "gap exit must not fabricate a fill at the unavailable stop price");
    require_near(closed.quantity, entry.quantity, "gap exit must close exactly the approved quantity");
    require(fixture.tick(98.0) == TradeAction::NONE, "cooldown must prevent an immediate follow-up entry");
    require(fixture.engine.get_total_trades() == 1, "repeated low price must not finalize the same trade twice");
}

void test_initial_stop_and_exact_trigger() {
    ReplayFixture fixture(risk_config(0.10));
    require(fixture.tick(100.0) == TradeAction::BUY, "initial-stop fixture must open a position");
    require(fixture.tick(101.0) == TradeAction::NONE, "initial-stop fixture must observe a favorable tick");
    require(fixture.tick(99.0) == TradeAction::SELL, "the original stop must still trigger after a small favorable move");
    require_near(fixture.engine.completed_trades().front().exit_price, 99.0, "exact-stop fill changed");
}

void test_confirmed_entry_costs_and_disabled_trailing() {
    auto risk = risk_config(0.10);
    risk.buy_fee_percent = 0.001;
    risk.spread_percent = 0.002;
    risk.slippage_percent = 0.001;
    ReplayFixture fixture(risk);
    require(fixture.tick(100.0) == TradeAction::BUY, "cost fixture must open a position");
    const auto entry = fixture.engine.get_current_position();
    require_near(entry.entry_price, 100.2001, "entry fill must retain spread and slippage");
    require_near(entry.stop_loss_price, entry.entry_price * 0.99, "initial protection must use the confirmed fill");
    require(fixture.tick(101.0) == TradeAction::NONE, "cost fixture must retain its favorable tick");
    auto position = fixture.engine.get_current_position();
    require_near(position.stop_loss_price, entry.stop_loss_price, "cost-adjusted protection must not be weakened");
    unchanged_approval(position, entry);
    require(fixture.tick(112.0) == TradeAction::NONE, "cost fixture must retain its higher tick");
    position = fixture.engine.get_current_position();
    require_near(position.stop_loss_price, 100.8, "cost-adjusted position must eventually trail");
    unchanged_approval(position, entry);

    risk.trailing_sl_enabled = false;
    risk.trailing_sl_percent = 0.0;
    ReplayFixture disabled(risk);
    require(disabled.tick(100.0) == TradeAction::BUY, "disabled-trailing fixture must open a position");
    const auto fixed_stop = disabled.engine.get_current_position().stop_loss_price;
    require(disabled.tick(120.0) == TradeAction::NONE, "disabled-trailing favorable tick must remain open");
    require_near(disabled.engine.get_current_position().stop_loss_price, fixed_stop,
                 "disabled trailing must leave initial protection unchanged");
}

nlohmann::json risk_json() {
    return {
        {"max_total_capital", 1000.0}, {"risk_per_trade", 0.005}, {"stop_loss_percent", 0.01},
        {"take_profit_percent", 0.50}, {"buy_fee_percent", 0.0}, {"sell_fee_percent", 0.0},
        {"slippage_percent", 0.0}, {"spread_percent", 0.0}, {"leverage", 1.0},
        {"min_quantity", 0.000001}, {"max_quantity", 0.0}, {"step_size", 0.000001},
        {"min_notional", 5.0}, {"cooldown_seconds", 30}, {"max_holding_seconds", 900},
        {"max_data_age_ms", 2000}, {"trailing_sl_enabled", true}, {"trailing_sl_percent", 0.10}
    };
}

RiskConfig load_fixture(const nlohmann::json& value) {
    std::ofstream output("risk.json");
    output << value;
    output.close();
    require(output.good(), "cannot write risk fixture");
    return load_risk_config("risk.json");
}

void test_file_configuration_boundaries() {
    require_near(load_fixture(risk_json()).trailing_sl_percent, 0.10,
                 "a wider valid trailing distance must remain supported");
    for (const auto& invalid : {nlohmann::json(-0.1), nlohmann::json(0.0), nlohmann::json(1.0),
                               nlohmann::json(1.1), nlohmann::json("0.01"), nlohmann::json(nullptr),
                               nlohmann::json(true), nlohmann::json::array()}) {
        auto value = risk_json();
        value["trailing_sl_percent"] = invalid;
        require_rejected([&] { load_fixture(value); }, "invalid enabled trailing distance was accepted");
    }
    auto value = risk_json();
    value.erase("trailing_sl_percent");
    require_rejected([&] { load_fixture(value); }, "enabled trailing must require a positive distance");
    value["trailing_sl_enabled"] = false;
    require_near(load_fixture(value).trailing_sl_percent, 0.0, "disabled trailing must allow an absent distance");
    value["trailing_sl_percent"] = -0.1;
    require_rejected([&] { load_fixture(value); }, "invalid dormant trailing distance must be rejected");
    value = risk_json();
    value["trailing_sl_enabled"] = "true";
    require_rejected([&] { load_fixture(value); }, "trailing flag must be a JSON boolean");
    for (double invalid : {0.0, -0.01, 1.0, 1.1}) {
        value = risk_json();
        value["stop_loss_percent"] = invalid;
        require_rejected([&] { load_fixture(value); }, "invalid initial stop distance was accepted");
    }
}

void test_direct_configurations_cannot_bypass_validation() {
    const double invalid_values[] = {0.0, -0.01, 1.0, 1.1,
        std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity()};
    for (double invalid : invalid_values) {
        auto risk = risk_config(invalid);
        require_rejected([&] { RiskManager manager(risk); }, "direct RiskManager accepted an invalid trailing policy");
        require_rejected([&] { ReplayFixture fixture(risk); }, "direct replay accepted an invalid trailing policy");
        risk = risk_config(0.01);
        risk.stop_loss_percent = invalid;
        require_rejected([&] { ReplayFixture fixture(risk); }, "direct replay accepted an invalid initial stop");
    }
    auto disabled = risk_config(0.0);
    disabled.trailing_sl_enabled = false;
    RiskManager allowed(disabled);
    (void)allowed;
}
} // namespace

int main() {
    try {
        TemporaryDirectory directory;
        sentum::runtime::RuntimeControl::global().pause_entries(false);
        (void)sentum::runtime::RuntimeControl::global().consume_manual_close();
        test_wider_trailing_distance_preserves_initial_protection();
        test_narrow_trailing_and_gap_preserve_executable_price();
        test_initial_stop_and_exact_trigger();
        test_confirmed_entry_costs_and_disabled_trailing();
        test_file_configuration_boundaries();
        test_direct_configurations_cannot_bypass_validation();
        std::cout << "trailing-stop risk tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "trailing-stop risk tests failed: " << error.what() << '\n';
        return 1;
    }
}
