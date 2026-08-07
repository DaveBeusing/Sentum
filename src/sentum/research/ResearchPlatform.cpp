#include <sentum/research/ResearchPlatform.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <stdexcept>
#include <utility>

#include <sentum/time/Clock.hpp>
#include <sentum/trader/TradeEngine.hpp>
#include <sentum/trader/strategy/MomentumStrategy.hpp>

namespace sentum::research {
namespace {

template <typename T>
std::vector<T> value_or(const nlohmann::json& object, const char* key, std::vector<T> fallback) {
    if (!object.contains(key)) return fallback;
    auto values = object.at(key).get<std::vector<T>>();
    if (values.empty()) throw std::runtime_error(std::string("Research grid '") + key + "' cannot be empty");
    return values;
}

void validate_positive(const std::vector<double>& values, const char* key, bool allow_zero = false) {
    for (double value : values) {
        if (!std::isfinite(value) || value < 0.0 || (!allow_zero && value == 0.0))
            throw std::runtime_error(std::string("Invalid research parameter in '") + key + "'");
    }
}

void validate_lookbacks(const std::vector<std::size_t>& values) {
    for (auto value : values)
        if (value < 2) throw std::runtime_error("Research lookback must be >= 2");
}

std::size_t checked_trial_count(const ResearchConfig& config) {
    if (config.max_trials == 0) throw std::runtime_error("max_trials must be >= 1");
    const std::size_t dimensions[] = {
        config.lookbacks.size(), config.entry_thresholds.size(), config.stop_losses.size(),
        config.take_profits.size(), config.slippages.size()
    };
    std::size_t count = 1;
    for (auto dimension : dimensions) {
        if (dimension == 0) throw std::runtime_error("Research parameter grid cannot be empty");
        if (count > config.max_trials / dimension)
            throw std::runtime_error("Research grid exceeds max_trials");
        count *= dimension;
    }
    return count;
}

BacktestMetrics average_metrics(const std::vector<BacktestMetrics>& values) {
    BacktestMetrics out;
    if (values.empty()) return out;
    double profit_factor_sum = 0.0;
    std::size_t finite_profit_factor = 0;
    for (const auto& m : values) {
        out.net_profit += m.net_profit;
        out.max_drawdown += m.max_drawdown;
        if (std::isfinite(m.profit_factor)) { profit_factor_sum += m.profit_factor; ++finite_profit_factor; }
        out.win_rate += m.win_rate;
        out.expectancy += m.expectancy;
        out.sharpe += m.sharpe;
        out.sortino += m.sortino;
        out.fee_share += m.fee_share;
        out.slippage_sensitivity += m.slippage_sensitivity;
        out.trades += m.trades;
    }
    const double n = static_cast<double>(values.size());
    out.net_profit /= n;
    out.max_drawdown /= n;
    out.profit_factor = finite_profit_factor ? profit_factor_sum / static_cast<double>(finite_profit_factor) : 0.0;
    out.win_rate /= n;
    out.expectancy /= n;
    out.sharpe /= n;
    out.sortino /= n;
    out.fee_share /= n;
    out.slippage_sensitivity /= n;
    return out;
}

void apply_trial_risk(RiskConfig& risk, const ParameterSet& parameters) {
    risk.stop_loss_percent = parameters.stop_loss_percent;
    risk.take_profit_percent = parameters.take_profit_percent;
    risk.slippage_percent = parameters.slippage_percent;
}

BacktestMetrics run_training_slice(const std::vector<MarketEvent>& events,
                                   std::size_t end,
                                   const ParameterSet& parameters,
                                   RiskConfig risk,
                                   const std::string& symbol) {
    if (end == 0 || end > events.size()) return {};
    apply_trial_risk(risk, parameters);
    auto clock = std::make_shared<ReplayClock>();
    TradeEngine engine(symbol, risk, clock,
        std::make_unique<MomentumStrategy>(parameters.lookback, parameters.entry_threshold), ":memory:");
    for (std::size_t i = 0; i < end; ++i) {
        clock->advance_to(events[i].timestamp);
        engine.process_event(events[i]);
    }
    return MetricsCalculator::calculate(engine.completed_trades());
}

BacktestMetrics run_validation_slice(const std::vector<MarketEvent>& events,
                                     std::size_t validation_begin,
                                     std::size_t validation_end,
                                     const ParameterSet& parameters,
                                     RiskConfig risk,
                                     const std::string& symbol) {
    if (validation_begin >= validation_end || validation_end > events.size()) return {};
    apply_trial_risk(risk, parameters);

    auto strategy = std::make_unique<MomentumStrategy>(parameters.lookback, parameters.entry_threshold);
    const std::size_t warmup = std::min<std::size_t>(validation_begin, parameters.lookback + 1);
    const std::size_t warmup_begin = validation_begin - warmup;
    for (std::size_t i = warmup_begin; i < validation_begin; ++i)
        strategy->on_price(events[i].price, events[i].timestamp);

    auto clock = std::make_shared<ReplayClock>();
    TradeEngine engine(symbol, risk, clock, std::move(strategy), ":memory:");
    for (std::size_t i = validation_begin; i < validation_end; ++i) {
        clock->advance_to(events[i].timestamp);
        engine.process_event(events[i]);
    }
    return MetricsCalculator::calculate(engine.completed_trades());
}

double finite_or_zero(double value) { return std::isfinite(value) ? value : 0.0; }

nlohmann::json metrics_json(const BacktestMetrics& metrics) {
    return {
        {"net_profit", finite_or_zero(metrics.net_profit)},
        {"max_drawdown", finite_or_zero(metrics.max_drawdown)},
        {"profit_factor", finite_or_zero(metrics.profit_factor)},
        {"win_rate", finite_or_zero(metrics.win_rate)},
        {"expectancy", finite_or_zero(metrics.expectancy)},
        {"sharpe", finite_or_zero(metrics.sharpe)},
        {"sortino", finite_or_zero(metrics.sortino)},
        {"fee_share", finite_or_zero(metrics.fee_share)},
        {"slippage_sensitivity", finite_or_zero(metrics.slippage_sensitivity)},
        {"trades", metrics.trades}
    };
}

nlohmann::json trial_json(const TrialResult& trial) {
    return {
        {"trial_id", trial.trial_id},
        {"parameters", {
            {"lookback", trial.parameters.lookback},
            {"entry_threshold", trial.parameters.entry_threshold},
            {"stop_loss_percent", trial.parameters.stop_loss_percent},
            {"take_profit_percent", trial.parameters.take_profit_percent},
            {"slippage_percent", trial.parameters.slippage_percent}
        }},
        {"train", metrics_json(trial.train)},
        {"validation", metrics_json(trial.validation)},
        {"train_score", finite_or_zero(trial.train_score)},
        {"validation_score", finite_or_zero(trial.validation_score)},
        {"overfit_gap", finite_or_zero(trial.overfit_gap)}
    };
}

} // namespace

ResearchConfig load_research_config(const std::string& path) {
    std::ifstream file(path);
    if (!file) throw std::runtime_error("Cannot open research config: " + path);
    nlohmann::json json;
    file >> json;

    ResearchConfig config;
    config.dataset = json.value("dataset", std::string{});
    config.symbol = json.value("symbol", std::string{});
    config.objective = json.value("objective", std::string("sharpe"));
    config.train_fraction = json.value("train_fraction", 0.70);
    config.walk_forward_folds = json.value("walk_forward_folds", std::size_t{3});
    config.max_trials = json.value("max_trials", std::size_t{5000});
    config.leaderboard_size = json.value("leaderboard_size", std::size_t{25});

    const nlohmann::json grid = json.contains("grid") ? json.at("grid") : nlohmann::json::object();
    if (!grid.is_object()) throw std::runtime_error("Research grid must be a JSON object");
    config.lookbacks = value_or<std::size_t>(grid, "lookback", {10, 20, 40});
    config.entry_thresholds = value_or<double>(grid, "entry_threshold", {0.0005, 0.001, 0.002});
    config.stop_losses = value_or<double>(grid, "stop_loss_percent", {});
    config.take_profits = value_or<double>(grid, "take_profit_percent", {});
    config.slippages = value_or<double>(grid, "slippage_percent", {});

    if (config.dataset.empty()) throw std::runtime_error("Research config requires dataset");
    if (config.symbol.empty()) throw std::runtime_error("Research config requires symbol");
    if (!(config.train_fraction > 0.10 && config.train_fraction < 0.95))
        throw std::runtime_error("train_fraction must be between 0.10 and 0.95");
    if (config.walk_forward_folds == 0) throw std::runtime_error("walk_forward_folds must be >= 1");
    if (config.max_trials == 0) throw std::runtime_error("max_trials must be >= 1");
    if (config.leaderboard_size == 0) throw std::runtime_error("leaderboard_size must be >= 1");
    validate_lookbacks(config.lookbacks);
    validate_positive(config.entry_thresholds, "entry_threshold", true);
    if (!config.stop_losses.empty()) validate_positive(config.stop_losses, "stop_loss_percent");
    if (!config.take_profits.empty()) validate_positive(config.take_profits, "take_profit_percent");
    if (!config.slippages.empty()) validate_positive(config.slippages, "slippage_percent", true);
    return config;
}

ResearchRunner::ResearchRunner(RiskConfig base_risk) : base_risk_(base_risk) {}

double ResearchRunner::score(const BacktestMetrics& metrics, const std::string& objective) {
    if (objective == "sharpe") return metrics.sharpe;
    if (objective == "sortino") return metrics.sortino;
    if (objective == "net_profit") return metrics.net_profit;
    if (objective == "profit_factor") return std::isfinite(metrics.profit_factor) ? metrics.profit_factor : 1000000.0;
    if (objective == "expectancy") return metrics.expectancy;
    if (objective == "risk_adjusted_profit") return metrics.net_profit / (1.0 + std::max(0.0, metrics.max_drawdown));
    throw std::runtime_error("Unsupported research objective: " + objective);
}

ResearchSummary ResearchRunner::run(const ResearchConfig& input_config) const {
    ResearchConfig config = input_config;
    if (config.stop_losses.empty()) config.stop_losses = {base_risk_.stop_loss_percent};
    if (config.take_profits.empty()) config.take_profits = {base_risk_.take_profit_percent};
    if (config.slippages.empty()) config.slippages = {base_risk_.slippage_percent};
    const auto expected_trials = checked_trial_count(config);

    const auto events = HistoricalEventReader::read_csv(config.dataset, config.symbol);
    if (events.size() < 20) throw std::runtime_error("Research dataset requires at least 20 events");

    std::size_t initial_train = static_cast<std::size_t>(static_cast<double>(events.size()) * config.train_fraction);
    initial_train = std::clamp<std::size_t>(initial_train, 2, events.size() - 1);
    const std::size_t remaining = events.size() - initial_train;
    const std::size_t folds = std::min(config.walk_forward_folds, remaining);
    if (folds == 0) throw std::runtime_error("Research dataset leaves no validation events");
    const std::size_t fold_width = std::max<std::size_t>(1, remaining / folds);

    ResearchSummary summary;
    summary.dataset = config.dataset;
    summary.symbol = config.symbol;
    summary.objective = config.objective;
    summary.generated_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    summary.events = events.size();
    summary.folds = folds;
    summary.trials = expected_trials;
    summary.results.reserve(expected_trials);

    std::size_t trial_id = 0;
    for (auto lookback : config.lookbacks)
    for (double entry_threshold : config.entry_thresholds)
    for (double stop_loss : config.stop_losses)
    for (double take_profit : config.take_profits)
    for (double slippage : config.slippages) {
        ParameterSet parameters{lookback, entry_threshold, stop_loss, take_profit, slippage};
        std::vector<BacktestMetrics> train_metrics;
        std::vector<BacktestMetrics> validation_metrics;
        train_metrics.reserve(folds);
        validation_metrics.reserve(folds);

        for (std::size_t fold = 0; fold < folds; ++fold) {
            const std::size_t train_end = initial_train + fold * fold_width;
            const std::size_t validation_end = fold + 1 == folds
                ? events.size()
                : std::min(events.size(), initial_train + (fold + 1) * fold_width);
            if (train_end >= validation_end) continue;

            train_metrics.push_back(run_training_slice(events, train_end, parameters, base_risk_, config.symbol));
            validation_metrics.push_back(run_validation_slice(events, train_end, validation_end,
                parameters, base_risk_, config.symbol));
        }

        TrialResult trial;
        trial.trial_id = ++trial_id;
        trial.parameters = parameters;
        trial.train = average_metrics(train_metrics);
        trial.validation = average_metrics(validation_metrics);
        trial.train_score = score(trial.train, config.objective);
        trial.validation_score = score(trial.validation, config.objective);
        trial.overfit_gap = trial.train_score - trial.validation_score;
        summary.results.push_back(std::move(trial));
    }

    summary.leaderboard = summary.results;
    std::stable_sort(summary.leaderboard.begin(), summary.leaderboard.end(), [](const auto& a, const auto& b) {
        if (a.validation_score != b.validation_score) return a.validation_score > b.validation_score;
        const double a_gap = std::abs(a.overfit_gap), b_gap = std::abs(b.overfit_gap);
        if (a_gap != b_gap) return a_gap < b_gap;
        return a.validation.trades > b.validation.trades;
    });
    if (summary.leaderboard.size() > config.leaderboard_size)
        summary.leaderboard.resize(config.leaderboard_size);
    return summary;
}

nlohmann::json ResearchRunner::to_json(const ResearchSummary& summary) {
    nlohmann::json json{
        {"dataset", summary.dataset},
        {"symbol", summary.symbol},
        {"objective", summary.objective},
        {"generated_at_ms", summary.generated_at_ms},
        {"events", summary.events},
        {"folds", summary.folds},
        {"trials", summary.trials},
        {"leaderboard", nlohmann::json::array()}
    };
    for (const auto& trial : summary.leaderboard) json["leaderboard"].push_back(trial_json(trial));
    return json;
}

void ResearchRunner::write_artifacts(const ResearchSummary& summary,
                                     const std::string& json_path,
                                     const std::string& csv_path) {
    const auto json_parent = std::filesystem::path(json_path).parent_path();
    const auto csv_parent = std::filesystem::path(csv_path).parent_path();
    if (!json_parent.empty()) std::filesystem::create_directories(json_parent);
    if (!csv_parent.empty()) std::filesystem::create_directories(csv_parent);

    const std::string json_tmp = json_path + ".tmp";
    {
        std::ofstream file(json_tmp, std::ios::trunc);
        if (!file) throw std::runtime_error("Cannot write research JSON artifact");
        file << to_json(summary).dump(2) << '\n';
    }
    std::error_code ec;
    std::filesystem::remove(json_path, ec);
    ec.clear();
    std::filesystem::rename(json_tmp, json_path, ec);
    if (ec) throw std::runtime_error("Cannot publish research JSON artifact: " + ec.message());

    std::ofstream csv(csv_path, std::ios::trunc);
    if (!csv) throw std::runtime_error("Cannot write research CSV artifact");
    csv << "trial_id,lookback,entry_threshold,stop_loss_percent,take_profit_percent,slippage_percent,"
           "train_score,validation_score,overfit_gap,train_trades,validation_trades,"
           "train_net_profit,validation_net_profit,train_max_drawdown,validation_max_drawdown,"
           "train_sharpe,validation_sharpe,train_sortino,validation_sortino\n";
    csv << std::setprecision(12);
    for (const auto& trial : summary.results) {
        csv << trial.trial_id << ',' << trial.parameters.lookback << ',' << trial.parameters.entry_threshold << ','
            << trial.parameters.stop_loss_percent << ',' << trial.parameters.take_profit_percent << ','
            << trial.parameters.slippage_percent << ',' << trial.train_score << ',' << trial.validation_score << ','
            << trial.overfit_gap << ',' << trial.train.trades << ',' << trial.validation.trades << ','
            << trial.train.net_profit << ',' << trial.validation.net_profit << ','
            << trial.train.max_drawdown << ',' << trial.validation.max_drawdown << ','
            << trial.train.sharpe << ',' << trial.validation.sharpe << ','
            << trial.train.sortino << ',' << trial.validation.sortino << '\n';
    }
}

} // namespace sentum::research
