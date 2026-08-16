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

## Correlation

Portfolio research derives return correlations for the selected assets and uses them when enforcing correlated-exposure limits. Datasets should use compatible sampling intervals and overlapping time periods for meaningful cross-asset comparisons.

## Output

The current portfolio result is written to:

```text
log/portfolio_research_latest.json
```

It includes per-asset metrics, candidate/accepted/rejected trades, realized volatility, correlation matrix, raw combined metrics and portfolio-filtered metrics.

For reproducible versioned runs, use the experiment manager described in [EXPERIMENT_DATASET_MANAGEMENT.md](EXPERIMENT_DATASET_MANAGEMENT.md).
