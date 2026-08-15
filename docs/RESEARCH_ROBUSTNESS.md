# Sentum Research Validation & Robustness

Phase 11 strengthens the Quant Research Platform so parameter selection and final performance estimation are separated.

## Data split

The dataset is divided into two regions:

1. **Research region** – expanding walk-forward folds are used for parameter selection.
2. **Final holdout** – the trailing `holdout_fraction` is never used to rank parameters. It is evaluated only once after an eligible winner has been selected.

`train_fraction` is measured against the complete dataset and must leave enough research-validation data before the holdout.

## Purging and embargo

For every fold, `purge_events` removes observations from the end of the training slice and `embargo_events` delays the start of validation execution. Strategy warmup can consume observations immediately before validation, but those observations never pass through `TradeEngine`, so they cannot create positions.

## Combined out-of-sample metrics

Validation trades from all folds are concatenated and passed once through `MetricsCalculator`. Sharpe, Sortino, drawdown, win rate, expectancy, profit factor, fees, and net profit therefore describe the combined OOS trade stream instead of an arithmetic mean of per-fold ratios.

## Eligibility and multiple testing

A trial is leaderboard-eligible only when it reaches `min_validation_trades`. Each trial also receives a conservative multiple-testing-adjusted Sharpe value. The current `deflated_sharpe` field is a transparent conservative approximation based on trial count and OOS trade count; it is not claimed to be the complete Bailey/Lopez de Prado Deflated Sharpe Ratio estimator.

## Parameter stability

Sentum calculates `parameter_stability_score` from nearby points in normalized parameter space. A broad plateau receives a better score than an isolated parameter spike with sharply worse neighbours.

## Final holdout

The final holdout is evaluated only for the selected eligible parameter set. Outputs include:

- holdout Net Profit, Max Drawdown, Sharpe, Sortino and other standard metrics
- deterministic bootstrap confidence interval for Net Profit
- deterministic Monte-Carlo trade resampling
- probability of a negative Monte-Carlo terminal result
- Monte-Carlo Net Profit and Max Drawdown confidence intervals
- performance grouped by simple market regimes

If no parameter set reaches `min_validation_trades`, the holdout remains unevaluated.

## Regime analysis

Trades are grouped using only information available before the entry timestamp. The initial heuristic labels `trending_up`, `trending_down`, `ranging`, `high_volatility`, or `unknown` using a short trailing price window. This is descriptive analysis, not a strategy input.

## Parallel execution

Independent parameter trials run through a bounded worker set. `parallelism: 0` uses `std::thread::hardware_concurrency`; a positive value caps the number of worker threads. Trial ordering and artifacts remain deterministic because results are written to preassigned trial indexes and all stochastic analysis uses `random_seed`.

## Example

```json
{
  "train_fraction": 0.60,
  "holdout_fraction": 0.15,
  "walk_forward_folds": 4,
  "purge_events": 20,
  "embargo_events": 5,
  "min_validation_trades": 20,
  "monte_carlo_samples": 5000,
  "bootstrap_samples": 5000,
  "confidence_level": 0.95,
  "random_seed": 9152026,
  "parallelism": 0
}
```

Run with:

```bash
./sentum research config/research.json
```

The legacy flag form remains supported:

```bash
./sentum --research config/research.json
```

The main artifact remains `log/research_latest.json`; complete trial data remains in `log/research_trials.csv`.
