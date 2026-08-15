# Sentum Quant Research Platform

Phase 10 adds a deterministic research layer on top of the existing replay, strategy, risk, and simulated-execution components. It is deliberately isolated from Binance Testnet and live runtime control paths.

## Goals

- reuse the same `TradeEngine`, `RiskManager`, simulated execution, fees, spread, slippage, and strategy implementation used by replay/paper trading
- run reproducible parameter experiments from JSON configuration
- evaluate parameters out-of-sample rather than ranking only on in-sample performance
- use expanding walk-forward validation with bounded lookback warmup
- retain every trial as CSV and publish a compact JSON leaderboard for dashboards and tooling
- prevent accidental combinatorial explosions with `max_trials`

## Run

Copy and edit the example configuration:

```bash
cp config/research.example.json config/research.json
./sentum research config/research.json
```

The legacy flag form remains supported:

```bash
./sentum --research config/research.json
```

Input data uses the normal replay CSV format:

```text
timestamp_ms,price,volume
1710000000000,68250.10,0.25
1710000001000,68260.20,0.12
```

The volume column is optional.

## Configuration

```json
{
  "dataset": "data/btcusdt.csv",
  "symbol": "BTCUSDT",
  "objective": "sharpe",
  "train_fraction": 0.70,
  "walk_forward_folds": 3,
  "max_trials": 5000,
  "leaderboard_size": 25,
  "grid": {
    "lookback": [10, 20, 40],
    "entry_threshold": [0.0005, 0.001, 0.002],
    "stop_loss_percent": [0.01, 0.02],
    "take_profit_percent": [0.02, 0.04],
    "slippage_percent": [0.00025, 0.0005]
  }
}
```

If stop loss, take profit, or slippage are omitted, the values from `config/risk.json` are used. The strategy grid currently targets `MomentumStrategy` and its `lookback` and `entry_threshold` parameters.

Supported objectives:

- `sharpe`
- `sortino`
- `net_profit`
- `profit_factor`
- `expectancy`
- `risk_adjusted_profit` (`net_profit / (1 + max_drawdown)`)

## Walk-forward validation

The first `train_fraction` of the event stream forms the initial training window. The remaining events are divided into validation folds. For each subsequent fold the training window expands to include the prior validation period.

Each validation replay starts only far enough before the validation boundary to warm up the strategy's lookback state. Trades are counted as validation trades only when their entry timestamp is inside the validation interval. This prevents the leaderboard from simply ranking the same in-sample trades used for parameter selection.

For every trial Sentum stores:

- averaged training metrics
- averaged validation metrics
- training objective score
- validation objective score
- `overfit_gap = train_score - validation_score`

The leaderboard is sorted primarily by validation score, then by the absolute overfit gap, then by validation trade count.

## Artifacts

A completed research run writes:

```text
log/research_latest.json
log/research_trials.csv
```

`research_latest.json` is the compact machine-readable leaderboard and is exposed read-only by the dashboard API at:

```text
GET /api/research
```

`research_trials.csv` contains all parameter combinations so external tools such as Python, R, Excel, or notebooks can perform deeper sensitivity analysis without rerunning Sentum.

## Safety and reproducibility

Research mode does not construct a Binance execution venue, does not read Binance API credentials, and exposes no order-control surface. It uses the deterministic replay clock and simulated execution path only.

Results remain dependent on the quality and representativeness of the historical dataset, execution-model assumptions, fees, spread, slippage, parameter search space, and validation design. A high validation score is not evidence that a strategy is safe or profitable in production.
