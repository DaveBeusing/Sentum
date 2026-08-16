# Research validation and robustness

Sentum separates parameter selection from final performance estimation. The research pipeline is designed to reduce obvious leakage and selection bias while keeping all strategy execution on the normal deterministic replay stack.

## Data split

The dataset is divided into:

1. **Research region** — expanding walk-forward folds are used for parameter search and validation.
2. **Final holdout** — a trailing region that is not used for ranking candidates and is evaluated only after an eligible winner is selected.

If no candidate reaches the configured eligibility requirements, the holdout remains unevaluated.

## Purging, embargo and warmup

`purge_events` removes observations near the end of the training slice and `embargo_events` delays validation execution. Strategy warmup may consume earlier observations to initialize rolling state, but those events do not enter `TradeEngine` and cannot open positions before the validation boundary.

## Combined out-of-sample metrics

Validation trades from all folds are concatenated and passed through the normal metrics calculator. Sharpe, Sortino, drawdown, win rate, expectancy, profit factor, fees and net profit therefore describe the combined OOS trade stream.

## Eligibility and selection controls

Research can require `min_validation_trades` before a trial is eligible for selection. Each trial also records:

- training objective score
- validation objective score
- overfit gap
- parameter-stability score
- conservative multiple-testing-adjusted Sharpe information

The adjusted Sharpe value is intentionally documented as a conservative approximation rather than a claim of a complete statistical correction for all forms of multiple testing.

## Final holdout analysis

For the selected candidate Sentum reports standard performance metrics plus deterministic robustness analysis including:

- bootstrap confidence interval for net profit
- Monte-Carlo trade resampling
- probability of a negative terminal result
- Monte-Carlo net-profit and max-drawdown intervals
- performance grouped by market regime

All stochastic analysis uses a configured seed to preserve reproducibility.

## Regime analysis

Trades can be grouped into descriptive regimes such as trending up, trending down, ranging and high volatility. Classification uses only information available before entry time and is intended for analysis rather than hidden future-aware strategy input.

## Parallel execution

Independent parameter trials can run through a bounded worker set. `parallelism: 0` uses available hardware concurrency; a positive value caps the number of workers. Result slots and random seeds remain deterministic so trial ordering does not depend on worker completion order.
