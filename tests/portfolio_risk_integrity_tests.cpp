#include <sentum/research/PortfolioResearch.hpp>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace std::chrono_literals;
using TimePoint = std::chrono::system_clock::time_point;
using sentum::research::PortfolioReturnEstimator;
constexpr long double seconds_per_year = 365.0L * 86400.0L;
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

class TemporaryDirectory {
public:
    TemporaryDirectory() : previous_(std::filesystem::current_path()) {
        auto pattern = (std::filesystem::temp_directory_path() / "sentum-portfolio-integrity-XXXXXX").string();
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');
        const char* created = ::mkdtemp(buffer.data());
        if (!created) throw std::runtime_error("Cannot create portfolio test directory");
        path_ = created;
        std::filesystem::current_path(path_);
        std::filesystem::create_directory("log");
    }
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::current_path(previous_, error);
        std::filesystem::remove_all(path_, error);
    }
private:
    std::filesystem::path path_, previous_;
};

std::vector<MarketEvent> history(const std::vector<double>& prices, const std::vector<int>& seconds) {
    require(prices.size() == seconds.size(), "invalid test history");
    std::vector<MarketEvent> result;
    for (std::size_t i = 0; i < prices.size(); ++i) {
        MarketEvent event;
        event.symbol = "BTC";
        event.price = event.close = prices[i];
        event.timestamp = epoch + std::chrono::seconds(seconds[i]);
        result.push_back(event);
    }
    return result;
}

std::vector<double> prices_from_returns(const std::vector<double>& returns) {
    std::vector<double> prices{100.0};
    for (const auto value : returns) prices.push_back(prices.back() * std::exp(value));
    return prices;
}

void strict_causal_cutoffs() {
    PortfolioReturnEstimator estimator;
    estimator.add_history("BTC", history({100, 101, 99, 10000}, {0, 1, 2, 3}));
    auto result = estimator.before(epoch + 2s);
    require(result.annualized_volatility.empty(), "the event at the decision timestamp leaked into volatility");
    require(result.evidence["volatility"]["BTC"]["return_intervals"] == 1, "strict cutoff included an extra interval");
    result = estimator.before(epoch + 3s);
    require(result.evidence["volatility"]["BTC"]["return_intervals"] == 2, "past estimator history is incomplete");
    require(result.evidence["volatility"]["BTC"]["window_end_ms"] == PortfolioReturnEstimator::milliseconds(epoch + 2s),
            "persisted estimator window must end strictly before the entry");
    PortfolioReturnEstimator prefix;
    prefix.add_history("BTC", history({100, 101, 99}, {0, 1, 2}));
    require(prefix.before(epoch + 3s).evidence == result.evidence, "future extreme price altered earlier statistics");
    require(estimator.before(epoch + 3s).evidence == result.evidence, "same-time decisions changed estimator state");
    rejects([&] { estimator.before(epoch + 2s); }, "backward estimator time was silently accepted");
    rejects([&] { estimator.add_history("ETH", history({100, 101}, {0, 1})); }, "late history mutation was accepted");
}

void interval_alignment() {
    PortfolioReturnEstimator estimator;
    const auto prices = prices_from_returns({0.01, 0.02, -0.01, 0.03});
    estimator.add_history("A", history(prices, {0, 1, 2, 3, 4}));
    estimator.add_history("DISJOINT", history(prices, {10, 11, 12, 13, 14}));
    estimator.add_history("COARSE", history(prices_from_returns({0.01, 0.03}), {0, 2, 4}));
    estimator.add_history("MISSING", history(prices_from_returns({0.01, 0.50, 0.03}), {0, 1, 3, 4}));
    estimator.add_history("INVERSE", history(prices_from_returns({-0.01, -0.02, 0.01, -0.03}), {0, 1, 2, 3, 4}));
    estimator.add_history("FLAT", history({100, 100, 100, 100, 100}, {0, 1, 2, 3, 4}));
    const auto result = estimator.before(epoch + 20s);
    const auto& row = result.correlations.at("A");
    require(!row.count("DISJOINT") && !row.count("COARSE") && !row.count("FLAT"),
            "unavailable correlation was fabricated from row offsets, unequal intervals or constant values");
    require(result.evidence["correlation"]["A"]["DISJOINT"]["return_intervals"] == 0,
            "disjoint histories should have no matched return intervals");
    require(result.evidence["correlation"]["A"]["MISSING"]["return_intervals"] == 2,
            "a gap must not pair unrelated return intervals");
    near(row.at("MISSING"), 1.0, "matching intervals were not correlated correctly across missing observations");
    near(row.at("INVERSE"), -1.0, "negative aligned correlation was not retained");
    near(result.correlations.at("INVERSE").at("A"), -1.0, "correlation matrix lost symmetry");
    near(result.annualized_volatility.at("FLAT"), 0.0, "flat history should have zero measured volatility");
}

void time_aware_annualization() {
    const std::vector<double> returns{0.01, -0.01, 0.02, -0.02};
    const auto prices = prices_from_returns(returns);
    const double sample_variance = (0.01 * 0.01 + 0.01 * 0.01 + 0.02 * 0.02 + 0.02 * 0.02) / 3.0;
    for (const int spacing : {1, 60, 3600, 86400}) {
        PortfolioReturnEstimator estimator;
        estimator.add_history("BTC", history(prices, {0, spacing, 2 * spacing, 3 * spacing, 4 * spacing}));
        const auto result = estimator.before(epoch + std::chrono::seconds(5 * spacing));
        const double expected = std::sqrt(sample_variance * static_cast<double>(seconds_per_year) / spacing);
        near(result.annualized_volatility.at("BTC"), expected, "annualization ignored the sampling interval");
    }

    // Irregular intervals use drift per unit time and standardized residuals.
    const std::vector<double> irregular_returns{0.01, -0.02, 0.03};
    const std::vector<double> durations{1, 4, 9};
    const double drift = (0.01 - 0.02 + 0.03) / 14.0;
    double residual_sum = 0.0;
    for (std::size_t i = 0; i < durations.size(); ++i)
        residual_sum += std::pow(irregular_returns[i] - drift * durations[i], 2) / durations[i];
    PortfolioReturnEstimator estimator;
    estimator.add_history("BTC", history(prices_from_returns(irregular_returns), {0, 1, 5, 14}));
    near(estimator.before(epoch + 15s).annualized_volatility.at("BTC"),
         std::sqrt(residual_sum / 2 * static_cast<double>(seconds_per_year)),
         "irregular intervals were silently treated as one-second returns");

    for (const auto& timestamps : {std::vector<int>{0, 0, 1}, std::vector<int>{0, 2, 1}}) {
        PortfolioReturnEstimator invalid;
        rejects([&] { invalid.add_history("BTC", history({100, 101, 102}, timestamps)); },
                "non-increasing timestamps must not silently enter return estimates");
    }
    PortfolioReturnEstimator invalid;
    rejects([&] { invalid.add_history("BTC", history({100, std::numeric_limits<double>::infinity()}, {0, 1})); },
            "non-finite portfolio history was accepted");
}

void write_prices(const std::string& path, const std::vector<double>& prices, double scale = 1.0) {
    std::ofstream file(path);
    file << "timestamp_ms,price\n" << std::setprecision(17);
    for (std::size_t i = 0; i < prices.size(); ++i)
        file << PortfolioReturnEstimator::milliseconds(epoch + std::chrono::hours(24 * i)) << ',' << prices[i] * scale << '\n';
    require(static_cast<bool>(file), "failed to write portfolio fixture");
}

RiskConfig risk_config() {
    RiskConfig risk;
    risk.max_total_capital = 1000;
    risk.risk_per_trade = 0.01;
    risk.stop_loss_percent = 0.02;
    risk.take_profit_percent = 0.04;
    risk.step_size = 0.001;
    risk.buy_fee_percent = risk.sell_fee_percent = risk.slippage_percent = risk.spread_percent = 0.0;
    risk.cooldown_seconds = 0;
    risk.max_holding_seconds = 500 * 86400;
    return risk;
}

void runner_prefix_invariance() {
    const std::vector<double> prefix{100, 99, 100, 101, 106, 100, 99, 100, 101, 102};
    auto extended = prefix;
    extended.insert(extended.end(), {103, 110, 10000, 10, 8000, 20});
    auto alternate = prefix;
    alternate.insert(alternate.end(), {90, 80, 75, 70, 65, 60});
    sentum::research::PortfolioResearchConfig config;
    config.starting_equity = 1000;
    config.datasets = {{"btc.csv", "BTC", 1.0}, {"eth.csv", "ETH", 1.0}};
    config.strategy = {{"type", "momentum"}, {"parameters", {{"lookback", 4}, {"entry_threshold", 0.005}}}};
    config.portfolio_risk.target_volatility = 0.20;
    config.portfolio_risk.max_asset_exposure = 1.0;
    config.portfolio_risk.max_gross_exposure = config.portfolio_risk.max_correlated_exposure = 2.0;
    sentum::research::PortfolioResearchRunner runner(risk_config());

    write_prices("btc.csv", prefix);
    write_prices("eth.csv", prefix, 2.0);
    const auto base = runner.run(config);
    require(base.decisions.size() == 4 && base.accepted_trades == 4 && base.open_accepted_trades == 2,
            "prefix fixture must include completed and still-open accepted entries on both symbols");
    require(base.portfolio_filtered.trades == 2, "open entries must not fabricate completed portfolio trades");
    require(base.decisions[0]["size_multiplier"].get<double>() > 0.2 && base.decisions[0]["size_multiplier"].get<double>() < 1.5,
            "prefix fixture must exercise volatility sizing away from both clamp limits");
    const auto last_prefix = PortfolioReturnEstimator::milliseconds(epoch + std::chrono::hours(24 * (prefix.size() - 1)));
    for (const bool reverse : {false, true}) {
        write_prices("btc.csv", reverse ? alternate : extended);
        write_prices("eth.csv", reverse ? extended : alternate, 2.0);
        const auto changed = runner.run(config);
        nlohmann::json early = nlohmann::json::array();
        for (const auto& decision : changed.decisions)
            if (decision.at("entry_time_ms").get<std::int64_t>() <= last_prefix) early.push_back(decision);
        require(early == base.decisions,
                "changing only future prices changed an earlier real Runner decision, size or estimator evidence");
        require(changed.descriptive_statistics != base.descriptive_statistics,
                "future fixture must actually change full-history descriptive statistics");
    }
    for (const auto& decision : base.decisions) {
        const auto cutoff = decision["entry_time_ms"].get<std::int64_t>();
        require(decision["statistics"]["cutoff_exclusive_ms"] == cutoff, "entry and estimator cutoff lost their binding");
        for (const auto& item : decision["statistics"]["volatility"].items())
            if (!item.value()["window_end_ms"].is_null())
                require(item.value()["window_end_ms"].get<std::int64_t>() < cutoff, "a risk window includes its entry event");
        require(decision["approved_notional"].get<double>() <= decision["equity"].get<double>(),
                "actual Runner acceptance exceeded its asset exposure cap");
        require(decision["accepted_notional"].get<double>() <= decision["approved_notional"].get<double>(),
                "quantity conversion rounded accepted exposure above its approved amount");
    }
    const auto json = sentum::research::PortfolioResearchRunner::to_json(base);
    require(json["decisions"] == base.decisions && json["open_accepted_trades"] == 2,
            "portfolio serialization omitted decision or outstanding-entry evidence");
    sentum::research::PortfolioResearchRunner::write_artifact(base, "portfolio.json");
    nlohmann::json persisted;
    std::ifstream("portfolio.json") >> persisted;
    require(persisted["decisions"] == json["decisions"], "persisted decision evidence changed");
}

} // namespace

int main() {
    try {
        TemporaryDirectory directory;
        strict_causal_cutoffs();
        interval_alignment();
        time_aware_annualization();
        runner_prefix_invariance();
        std::cout << "portfolio risk integrity tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "portfolio risk integrity tests failed: " << error.what() << '\n';
        return 1;
    }
}
