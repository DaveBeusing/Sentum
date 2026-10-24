# Strategy and portfolio research

Sentum provides a shared strategy framework for Paper, replay, shadow and research workflows. Strategies are created from JSON through `sentum::strategy::StrategyFactory` and execute through the same risk and simulated-execution components used elsewhere in the application.

## Supported strategies

The built-in strategy types are:

- `momentum`
- `trend`
- `mean_reversion`
- `breakout`
- `multi_timeframe_trend`
- `ensemble`

Each strategy exposes a signal action, strategy name, explanation, reference price, timestamp and confidence value.

## Multi-timeframe strategies

`TimeframeAggregator` derives larger bars from the incoming market-event stream. Multi-timeframe strategies can therefore combine short and slow contexts without requiring a second market-data implementation.

## Ensembles

An ensemble combines weighted strategy members and requires the normalized aggregate confidence to reach a configured threshold. Example:

```json
{
  "type": "ensemble",
  "threshold": 0.55,
  "members": [
    {
      "type": "momentum",
      "weight": 1.0,
      "parameters": {"lookback": 20, "entry_threshold": 0.001}
    },
    {
      "type": "trend",
      "weight": 1.0,
      "parameters": {"fast_period": 12, "slow_period": 26, "threshold": 0.001}
    }
  ]
}
```

## Portfolio research

`sentum_portfolio_research` evaluates multiple assets using the shared strategy factory and trading engine, then applies portfolio-level risk controls to the resulting candidate trades.

```bash
cp config/portfolio-research.example.json config/portfolio-research.json
./build/sentum_portfolio_research config/portfolio-research.json
```

The portfolio layer can enforce controls such as:

- maximum gross exposure
- per-asset exposure
- correlated exposure
- daily drawdown
- consecutive-loss limits
- trades per hour
- volatility-targeted sizing

### Final exposure approval

The portfolio risk boundary calculates the volatility multiplier first, clamps it to
`minimum_size_multiplier` and `maximum_size_multiplier`, and checks the resulting
notional against every gross, asset and correlated exposure limit. The runner
converts that approved final notional into quantity, rounding toward the budget
if floating-point division would otherwise exceed it. A valid downsized proposal can pass even
when its original amount exceeded a limit; an upsized proposal cannot exceed a
limit that was checked only before scaling. Rejections expose no approved amount.

Both multiplier bounds can be supplied in `portfolio_risk`; their defaults are
`0.20` and `1.50`. Limits and estimator inputs must be finite, and multiplier
bounds must be positive and ordered. Invalid configuration is rejected before a
research run starts. Invalid snapshot values fail the risk decision closed.

### Causal decision statistics

Each entry uses expanding history containing only return intervals whose **end
timestamp is strictly before the entry timestamp**. The entry event and all later
events are excluded from both volatility and correlation estimation. The
estimator advances incrementally as entries are processed in timestamp/symbol
order; it does not repeatedly rescan the entire history.

Changing or appending future prices therefore cannot change an earlier entry's
statistics, approval or quantity. An entry that is still open at the end of its
dataset is also evaluated. Its existence does not depend on a future closing
price. Such an accepted entry remains open and contributes no fabricated realized
profit to completed-trade metrics.

Each decision records its symbol, entry timestamp, equity, proposed notional,
approval/reason, multiplier, approved final quantity/notional, estimator version,
exclusive cutoff, observation counts and the first/last interval timestamps used
for every estimate. These records are the statistics used for admission. The
top-level asset volatility and correlation matrix describe the complete dataset
and are not fed back into historical decisions.

### Volatility and sampling time

Volatility uses log returns and the actual elapsed seconds of each interval. The
estimator fits a drift per second and measures standardized residual variance:

```text
drift = sum(log_return) / sum(interval_seconds)
variance_rate = sum((log_return - drift * interval_seconds)^2 / interval_seconds) / (n - 1)
annualized_volatility = sqrt(variance_rate * 365 * 24 * 60 * 60)
```

For equally spaced observations this is ordinary sample log-return variance
annualized using that sampling interval. Irregular positive intervals use the
same elapsed-time model explicitly. At least two return intervals are required.
A constant-price history has measured zero volatility. Missing or zero volatility
uses multiplier `1.0`, constrained by the configured multiplier bounds, instead of
using future observations to fill the warmup period.

Portfolio datasets require finite positive prices and strictly increasing
timestamps after CSV ordering. Duplicate timestamps are rejected because they do
not define a positive sampling interval. The model assumes a 365-day year; it does
not infer an exchange trading calendar or claim that an annualized estimate
predicts future volatility.

### Correlation and missing observations

Correlations use pairs of log returns with **identical start and end timestamps**.
An equal row number, equal endpoint alone, or overlapping date range is not a
matching return interval. Missing observations and different sampling intervals
do not silently pair unrelated returns. At least two matching, nonconstant return
pairs are required for a Pearson correlation; otherwise the estimate is explicitly
unavailable and is omitted from the numeric matrix. Its evidence still records
availability and the matched interval count.

Unknown correlation does not grant diversification credit: existing exposure
with an unavailable correlation consumes the correlated-exposure budget. Exposure
to the same symbol is always correlated. When an estimate is available, the
configured absolute-correlation threshold determines which other positions count
toward that budget. Datasets with compatible sampling and overlapping history
produce more usable correlation evidence.

## Output

The current portfolio result is written to:

```text
log/portfolio_research_latest.json
```

It includes per-asset metrics, candidate/accepted/rejected entries, descriptive
volatility/correlation statistics, raw combined metrics, portfolio-filtered
completed-trade metrics, and the complete `decisions` evidence array.
`candidate_trades` and `accepted_trades` include entries still open at the dataset
boundary; `open_accepted_trades` reports that outstanding subset. The metrics'
`trades` counts include completed trades only. `volatility_available` distinguishes
an insufficient-history default from an observed zero. The
`descriptive_statistics` object records the windows/counts behind the full-history
summary.

This workflow generates independent per-asset candidates through the shared
trading engine, then applies the portfolio admission layer. It does not execute
exchange orders. The separate portfolio daily-baseline rollover and exit-order
accounting limitations remain; these estimator and exposure corrections alone do
not qualify the complete portfolio research workflow.

For reproducible versioned runs, use the experiment manager described in [EXPERIMENT_DATASET_MANAGEMENT.md](EXPERIMENT_DATASET_MANAGEMENT.md).
