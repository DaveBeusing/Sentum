# Phase 18 - Interactive Paper Trading & Strategy Control

Phase 18 turns the Phase-17 terminal monitor into a controllable paper-trading runtime while preserving simulated execution and fail-closed model promotion.

## Paper runtime

Paper mode uses live Binance market data but never submits exchange orders. Entries and exits run through `SimulatedExecutionVenue`, including configured spread, slippage and fees.

The runtime strategy is created by the same `sentum::strategy::StrategyFactory` used by research and model workflows. Supported strategy types are:

- `momentum`
- `trend`
- `mean_reversion`
- `breakout`
- `multi_timeframe_trend`
- `ensemble`

The legacy duplicate strategy factory has been removed.

## Configuration

Use `config/config.phase18.example.json` as a reference. The relevant sections are:

```json
{
  "paperTrading": true,
  "paper": {
    "initialBalance": 10000.0,
    "statePath": "log/paper_account.json",
    "autoSymbol": true,
    "symbol": "",
    "modelDefinition": ""
  },
  "strategy": {
    "type": "momentum",
    "parameters": {
      "lookback": 20,
      "entry_threshold": 0.001
    }
  }
}
```

`paper.initialBalance` is simulation capital. The runtime no longer uses the real Binance balance as paper equity.

`paper.statePath` persists paper equity and realized P/L across restarts.

`paper.autoSymbol=true` lets the scanner choose the initial trading symbol. Set it to `false` and provide `paper.symbol` to run a fixed symbol.

## Promoted model -> Paper

Set `paper.modelDefinition` to a Phase-15 model definition JSON. Sentum opens `log/models.sqlite3` and requires that model's current stage to be exactly `paper`.

When accepted, the model definition supplies:

- model strategy JSON
- model symbol
- model risk-config path metadata

The runtime switches to manual-symbol mode for the promoted model. A research/shadow/testnet-stage model cannot be loaded into Paper through this path.

## TUI controls

Paper mode starts the TUI when stdout is a terminal.

- `S` - cycle strategy preset. Change is applied only between positions.
- `A` - scanner/automatic symbol selection.
- `M` - enter a manual symbol, then press Enter. Escape cancels.
- `P` - pause/resume **new entries**. Open positions continue to receive stop-loss, take-profit and maximum-holding-time management.
- `C` - request a manual paper close through `SimulatedExecutionVenue`.
- `Ctrl+C` - stop Sentum.

Strategy and symbol changes are not applied while a position is open. They are marked pending and applied after the position is closed.

## Runtime explainability

The shared `DashboardState` now carries strategy/paper state including:

- strategy name/config
- symbol mode/manual symbol
- entry-pause state
- last signal
- signal confidence/reason
- last risk decision/reason
- active position strategy/signal/risk provenance
- paper account equity and realized P/L
- last exit reason and last trade P/L

The TUI and web dashboard therefore continue to share one presentation state.

## Persistent paper account

`PaperAccount` stores a small JSON state file atomically. It tracks:

- initial balance
- current equity
- realized P/L
- closed-trade count
- quote currency

The paper account is independent from the authenticated Binance account balance.

## Safety properties

- live exchange order execution remains disabled in Paper
- pause means **no new entries**, not unmanaged open positions
- manual closes use the normal simulated execution path
- strategy/symbol changes wait for an open position to exit
- promoted model loading requires registry stage `paper`
- production trading is not introduced by Phase 18
