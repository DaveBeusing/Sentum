#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <sentum/backtest/Backtest.hpp>
#include <sentum/trader/types/RiskConfig.hpp>
#include <sentum/trader/types/TradePosition.hpp>

namespace sentum::research {

struct ParameterSet {
    std::size_t lookback = 20;
    double entry_threshold = 0.001;
    double stop_loss_percent = 0.02;
    double take_profit_percent = 0.04;
    double slippage_percent = 0.0005;
};

struct ResearchConfig {
    std::string dataset;
    std::string symbol;
    std::string objective = "sharpe";
    double train_fraction = 0.60;
    double holdout_fraction = 0.15;
    std::size_t walk_forward_folds = 3;
    std::size_t purge_events = 0;
    std::size_t embargo_events = 0;
    std::size_t min_validation_trades = 10;
    std::size_t max_trials = 5000;
    std::size_t leaderboard_size = 25;
    std::size_t monte_carlo_samples = 2000;
    std::size_t bootstrap_samples = 2000;
    double confidence_level = 0.95;
    std::uint64_t random_seed = 0x53454e54554dULL;
    std::size_t parallelism = 0; // 0 = hardware_concurrency
    std::vector<std::size_t> lookbacks{10, 20, 40};
    std::vector<double> entry_thresholds{0.0005, 0.001, 0.002};
    std::vector<double> stop_losses;
    std::vector<double> take_profits;
    std::vector<double> slippages;
};

struct ConfidenceInterval {
    double lower = 0.0;
    double median = 0.0;
    double upper = 0.0;
};

struct MonteCarloSummary {
    std::size_t samples = 0;
    ConfidenceInterval net_profit;
    ConfidenceInterval max_drawdown;
    double probability_of_loss = 0.0;
};

struct RegimeMetrics {
    std::string regime;
    BacktestMetrics metrics;
};

struct TrialResult {
    std::size_t trial_id = 0;
    ParameterSet parameters;
    BacktestMetrics train;
    BacktestMetrics validation;
    double train_score = 0.0;
    double validation_score = 0.0;
    double overfit_gap = 0.0;
    double parameter_stability_score = 0.0;
    double deflated_sharpe = 0.0;
    bool eligible = false;
};

struct ResearchSummary {
    std::string dataset;
    std::string symbol;
    std::string objective;
    std::int64_t generated_at_ms = 0;
    std::size_t events = 0;
    std::size_t research_events = 0;
    std::size_t holdout_events = 0;
    std::size_t folds = 0;
    std::size_t trials = 0;
    std::vector<TrialResult> results;
    std::vector<TrialResult> leaderboard;
    bool holdout_evaluated = false;
    ParameterSet selected_parameters;
    BacktestMetrics final_holdout;
    double final_holdout_score = 0.0;
    ConfidenceInterval bootstrap_net_profit;
    MonteCarloSummary monte_carlo;
    std::vector<RegimeMetrics> holdout_regimes;
};

ResearchConfig load_research_config(const std::string& path);

class ResearchRunner {
public:
    explicit ResearchRunner(RiskConfig base_risk);
    ResearchSummary run(const ResearchConfig& config) const;

    static double score(const BacktestMetrics& metrics, const std::string& objective);
    static nlohmann::json to_json(const ResearchSummary& summary);
    static void write_artifacts(const ResearchSummary& summary,
                                const std::string& json_path = "log/research_latest.json",
                                const std::string& csv_path = "log/research_trials.csv");

private:
    RiskConfig base_risk_;
};

} // namespace sentum::research
