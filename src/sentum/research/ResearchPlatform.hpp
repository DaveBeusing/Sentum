#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <sentum/backtest/Backtest.hpp>
#include <sentum/trader/types/RiskConfig.hpp>

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
    double train_fraction = 0.70;
    std::size_t walk_forward_folds = 3;
    std::size_t max_trials = 5000;
    std::size_t leaderboard_size = 25;
    std::vector<std::size_t> lookbacks{10, 20, 40};
    std::vector<double> entry_thresholds{0.0005, 0.001, 0.002};
    std::vector<double> stop_losses;
    std::vector<double> take_profits;
    std::vector<double> slippages;
};

struct TrialResult {
    std::size_t trial_id = 0;
    ParameterSet parameters;
    BacktestMetrics train;
    BacktestMetrics validation;
    double train_score = 0.0;
    double validation_score = 0.0;
    double overfit_gap = 0.0;
};

struct ResearchSummary {
    std::string dataset;
    std::string symbol;
    std::string objective;
    std::int64_t generated_at_ms = 0;
    std::size_t events = 0;
    std::size_t folds = 0;
    std::size_t trials = 0;
    std::vector<TrialResult> results;
    std::vector<TrialResult> leaderboard;
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
