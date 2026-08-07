# Sentum Strategy Framework & Portfolio Research

Phase 12 turns the single-strategy research stack into a reusable strategy framework and adds multi-asset portfolio research.

## Strategy framework

Strategies implement `IStrategy` and can consume either simple prices or full `MarketEvent` values through `on_event()`.

Built-in strategies:

- `momentum`
- `trend`
- `mean_reversion`
- `breakout`
- `multi_timeframe_trend`
- `ensemble`

`StrategyFactory` constructs strategies from JSON. Ensemble strategies recursively construct weighted child strategies and emit a BUY signal only when their weighted confidence reaches the configured threshold.

Example:

```json
{
  "type": "ensemble",
  "threshold": 0.55,
  "members": [
    {"type": "momentum", "weight": 1.0, "parameters": {"lookback": 20, "entry_threshold": 0.001}},
    {"type": "trend", "weight": 1.0, "parameters": {"fast_period": 12, "slow_period": 26, "threshold": 0.001}}
  ]
}
```

## Multi-timeframe strategy

`multi_timeframe_trend` aggregates the normal event stream into independent fast and slow time buckets. Only closed aggregate frames update the corresponding EMA state, so the strategy does not require a second market-data feed.

Example parameters:

```json
{
  "fast_timeframe_seconds": 60,
  "slow_timeframe_seconds": 300,
  "ema_period": 8,
  "threshold": 0.001
}
```

## Portfolio risk overlay

`PortfolioRiskManager` evaluates a proposed position against a portfolio snapshot and can reject it for:

- maximum gross exposure
- maximum per-asset exposure
- maximum correlated exposure
- daily drawdown limit
- consecutive-loss limit
- trades-per-hour limit

It also produces a volatility-targeting size multiplier bounded by configured minimum and maximum multipliers.

## Portfolio research

Build:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

Run:

```bash
cp config/portfolio-research.example.json config/portfolio-research.json
./build/sentum_portfolio_research config/portfolio-research.json
```

The runner:

1. loads each configured historical dataset,
2. instantiates the same strategy definition for each asset through `StrategyFactory`,
3. runs every asset through the normal deterministic `ReplayClock` + `TradeEngine` + `RiskManager` + simulated execution stack,
4. calculates per-asset return volatility and cross-asset return correlations,
5. merges candidate trades in entry-time order,
6. applies portfolio-level exposure, correlation, drawdown, loss-streak and trade-rate limits,
7. applies the volatility-targeted size multiplier to approved trades,
8. compares raw combined metrics with portfolio-filtered metrics.

Output:

```text
log/portfolio_research_latest.json
```

The artifact contains per-asset metrics, the correlation matrix, candidate/accepted/rejected trade counts and combined portfolio metrics.

## Interpretation

The portfolio layer is a research overlay, not an exchange account manager. It evaluates how a common strategy would have behaved across multiple assets under portfolio-level constraints. Production/Testnet execution continues to use the existing exchange-confirmed order pipeline.

For meaningful correlation results, datasets should cover the same sampling interval and broadly aligned timestamps. A future dataset-management phase should align series explicitly by timestamp rather than relying on comparable historical streams.
