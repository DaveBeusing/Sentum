#include <sentum/trader/risk/PortfolioRiskManager.hpp>

#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
using namespace std::chrono_literals;
using TimePoint = std::chrono::system_clock::time_point;
using sentum::risk::PortfolioRiskConfig;
using sentum::risk::PortfolioRiskManager;
using sentum::risk::PortfolioRiskSnapshot;
const TimePoint epoch{946684800s};

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void near(double actual, double expected, const std::string& message) {
    require(std::isfinite(actual) && std::abs(actual - expected) <= 1e-10 * std::max(1.0, std::abs(expected)), message);
}

template <typename F>
void rejects(F action, const std::string& message) {
    bool rejected = false;
    try { action(); } catch (const std::exception&) { rejected = true; }
    require(rejected, message);
}

PortfolioRiskSnapshot snapshot() {
    PortfolioRiskSnapshot value;
    value.equity = 100.0;
    value.day_start_equity = 100.0;
    value.annualized_volatility["BTC"] = 0.01;
    return value;
}

void final_exposure_boundaries() {
    auto state = snapshot();
    const auto original_failure = PortfolioRiskManager{}.approve("BTC", 30.0, state, epoch);
    require(!original_failure.approved && original_failure.reason == "asset exposure limit exceeded",
            "a 30% proposal scaled to 45% must not pass a 35% asset limit");

    for (const std::string kind : {"gross", "asset", "correlated"}) {
        PortfolioRiskConfig config;
        config.max_gross_exposure = config.max_asset_exposure = config.max_correlated_exposure = 2.0;
        if (kind == "gross") config.max_gross_exposure = 0.5;
        if (kind == "asset") config.max_asset_exposure = 0.5;
        if (kind == "correlated") config.max_correlated_exposure = 0.5;
        state = snapshot();
        state.positions = {{kind == "asset" ? "BTC" : "ETH", 20.0}};
        state.correlations["BTC"]["ETH"] = 1.0;
        PortfolioRiskManager manager(config);
        for (const double proposed : {std::nextafter(20.0, 0.0), 20.0, std::nextafter(20.0, 100.0)}) {
            const auto decision = manager.approve("BTC", proposed, state, epoch);
            require(decision.approved == (proposed <= 20.0), kind + " must check the final scaled limit exactly");
            if (decision.approved) {
                near(decision.size_multiplier, 1.5, kind + " boundary multiplier changed");
                near(decision.approved_notional, proposed * 1.5, kind + " approved amount changed");
                require(20.0L + decision.approved_notional <= 50.0L, kind + " accepted exposure exceeded its budget");
            } else require(decision.approved_notional == 0.0, "rejected decisions must not expose an approved amount");
        }
    }

    state = snapshot();
    state.annualized_volatility["BTC"] = 0.02;
    require(PortfolioRiskManager{}.approve("BTC", 35.0, state, epoch).approved,
            "the exact default 35% asset boundary must remain admissible");
    state.annualized_volatility["BTC"] = 0.04;
    PortfolioRiskConfig config;
    config.max_asset_exposure = 0.5;
    const auto downsized = PortfolioRiskManager(config).approve("BTC", 80.0, state, epoch);
    require(downsized.approved, "a valid downsized amount must not be rejected using its unscaled proposal");
    near(downsized.approved_notional, 40.0, "downsized final amount is wrong");

    config.minimum_size_multiplier = 0.4;
    config.maximum_size_multiplier = 1.2;
    state.annualized_volatility["BTC"] = 10.0;
    near(PortfolioRiskManager(config).approve("BTC", 50.0, state, epoch).approved_notional, 20.0,
         "minimum multiplier was not applied before exposure validation");
    state.annualized_volatility["BTC"] = 0.00001;
    near(PortfolioRiskManager(config).approve("BTC", 20.0, state, epoch).approved_notional, 24.0,
         "maximum multiplier was not applied before exposure validation");
}

void unknown_correlations_and_invalid_numbers() {
    PortfolioRiskConfig config;
    config.target_volatility = 0.0;
    config.max_asset_exposure = 1.0;
    config.max_correlated_exposure = 0.5;
    auto state = snapshot();
    state.positions = {{"ETH", 40.0}};
    require(!PortfolioRiskManager(config).approve("BTC", 20.0, state, epoch).approved,
            "unknown correlation must not be interpreted as diversification");
    state.correlations["BTC"]["ETH"] = 0.0;
    require(PortfolioRiskManager(config).approve("BTC", 20.0, state, epoch).approved,
            "known uncorrelated exposure should remain independent");
    state.positions = {{"BTC", 40.0}};
    state.correlations["BTC"]["BTC"] = 0.0;
    require(!PortfolioRiskManager(config).approve("BTC", 20.0, state, epoch).approved,
            "same-symbol inventory must always consume correlated exposure");

    const auto infinity = std::numeric_limits<double>::infinity();
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    for (const double bad : {nan, infinity, -infinity}) {
        state = snapshot();
        require(!PortfolioRiskManager{}.approve("BTC", bad, state, epoch).approved,
                "non-finite proposed notional passed the portfolio boundary");
        state.equity = bad;
        require(!PortfolioRiskManager{}.approve("BTC", 10.0, state, epoch).approved, "non-finite equity passed");
        state = snapshot(); state.annualized_volatility["BTC"] = bad;
        require(!PortfolioRiskManager{}.approve("BTC", 10.0, state, epoch).approved, "non-finite volatility passed");
        state = snapshot(); state.positions = {{"ETH", bad}};
        require(!PortfolioRiskManager{}.approve("BTC", 10.0, state, epoch).approved, "non-finite inventory passed");
        state = snapshot(); state.positions = {{"ETH", 10.0}}; state.correlations["BTC"]["ETH"] = bad;
        require(!PortfolioRiskManager{}.approve("BTC", 10.0, state, epoch).approved, "non-finite correlation passed");
        auto invalid = config; invalid.maximum_size_multiplier = bad;
        rejects([&] { PortfolioRiskManager manager(invalid); }, "non-finite multiplier configuration passed");
        invalid = config; invalid.max_asset_exposure = bad;
        rejects([&] { PortfolioRiskManager manager(invalid); }, "non-finite exposure configuration passed");
    }
    auto invalid = config; invalid.minimum_size_multiplier = 2.0; invalid.maximum_size_multiplier = 1.0;
    rejects([&] { PortfolioRiskManager manager(invalid); }, "reversed multiplier bounds passed");
    state = snapshot(); state.equity = std::numeric_limits<double>::max(); state.day_start_equity = state.equity;
    require(!PortfolioRiskManager{}.approve("BTC", state.equity, state, epoch).approved,
            "overflowing scaled notional passed");
}

} // namespace

int main() {
    try {
        final_exposure_boundaries();
        unknown_correlations_and_invalid_numbers();
        std::cout << "portfolio exposure tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "portfolio exposure tests failed: " << error.what() << '\n';
        return 1;
    }
}
