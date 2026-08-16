# Quant research

Sentum's research layer reuses the same strategy, risk and simulated-execution components used by replay and Paper trading. The goal is deterministic strategy evaluation without introducing a second simplified backtesting implementation.

## Run

```bash
cp config/research.example.json config/research.json
./sentum research config/research.json
```

Input data uses the replay CSV format:

```text
timestamp_ms,price,volume
1710000000000,68250.10,0.25
1710000001000,68260.20,0.12
```

Volume is optional.

## Parameter search

Research configuration defines a bounded Cartesian parameter grid. Typical dimensions include strategy lookback/threshold and execution assumptions such as stop loss, take profit and slippage. `max_trials` prevents accidental unbounded search spaces.

Supported objective functions include:

- `sharpe`
- `sortino`
- `net_profit`
- `profit_factor`
- `expectancy`
- `risk_adjusted_profit`

## Validation

Parameter selection uses expanding walk-forward evaluation. Training data is separated from out-of-sample validation and the final holdout. Strategy state may be warmed with preceding observations, but warmup observations do not pass through the trading engine and cannot create validation positions.

Validation trades from all folds are combined and metrics are recalculated over the resulting OOS trade stream rather than averaging fold-level ratios.

Trials may be filtered by minimum validation trade count and ranked using validation performance, overfit gap, parameter stability and conservative multiple-testing information.

See [RESEARCH_ROBUSTNESS.md](RESEARCH_ROBUSTNESS.md) for the full validation methodology.

## Strategies

The shared strategy framework supports momentum, trend, mean reversion, breakout, multi-timeframe trend and weighted ensembles. Portfolio research applies the same strategy definitions across multiple assets; see [STRATEGY_PORTFOLIO_RESEARCH.md](STRATEGY_PORTFOLIO_RESEARCH.md).

## Output

Direct research writes current result artifacts such as:

```text
log/research_latest.json
log/research_trials.csv
```

Managed experiments additionally persist immutable run directories and provenance through the experiment registry. See [EXPERIMENT_DATASET_MANAGEMENT.md](EXPERIMENT_DATASET_MANAGEMENT.md).

## Safety

Research mode does not create a Binance execution venue and does not submit exchange orders. Results remain dependent on dataset quality, execution assumptions, search-space design and validation methodology; a strong historical score is not evidence that a strategy will be profitable in future trading.
