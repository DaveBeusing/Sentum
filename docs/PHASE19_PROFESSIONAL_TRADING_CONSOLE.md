# Phase 19 - Professional Trading Console

Phase 19 turns the interactive paper TUI into a trading-first workstation while preserving the Phase-18 safety model and the shared `DashboardState` presentation source.

## Design goals

The default terminal experience should answer five questions immediately:

1. What market/symbol is Sentum currently trading?
2. Is a position open and what is its P/L, stop and target?
3. What strategy signal exists and why?
4. What risk decision was made?
5. Is the runtime/data path healthy?

Technical telemetry remains available, but no longer dominates the default screen.

## Tabs

Use keys `1` through `7`:

```text
[1] MARKET
[2] SCANNER
[3] ORDERS
[4] TRADES
[5] STRATEGY
[6] MODELS
[7] SYSTEM
```

### Market

The trading-first home screen shows:

- active symbol and current position price
- paper equity, realized and unrealized P/L
- position entry, quantity, stop loss and take profit
- configured risk per trade and holding limit
- indicative bid/ask derived from the configured simulated fill spread (not exchange L2 data)
- compact equity sparkline
- scanner watchlist
- recent completed trades

### Scanner

Shows the current market-performance ranking produced by `SymbolScanner`. Scanner ranking remains distinct from strategy approval: a high-ranked symbol is a candidate, not automatically a BUY signal.

### Orders

Reads recent persisted order events through the existing read-only `DashboardRepository` and displays requested/executed quantity, fill price, order state and source.

### Trades

Shows recent completed trades including entry/exit, fees, net P/L and exit reason, plus the current session equity sparkline.

### Strategy

Shows:

- selected strategy
- last action
- real aggregate confidence
- last risk decision
- signal and risk reasons
- strategy configuration

For ensemble strategies, configured members and weights are shown. Member confirmation is derived from the actual ensemble signal reason; Sentum does not invent per-member confidence values that the engine does not export.

### Models

Shows registered model IDs/names, symbol and current promotion stage from `log/models.sqlite3`. Model promotion remains read-only from the TUI; stage changes still require the explicit Phase-15 CLI promotion workflow.

### System

Contains the operational details moved out of the main trading view:

- collector/scanner/trader health
- events/second
- persistence queue depth/drop rate
- database path/size
- parser, event-dispatch, strategy-decision and SQLite batch p50/p95/p99/max latency
- dashboard bind address

## Trading controls

Phase-18 controls remain global across tabs:

```text
[S] cycle strategy preset
[A] scanner auto-symbol mode
[M] enter manual symbol
[P] pause/resume new entries
[C] close current paper position through SimulatedExecutionVenue
[Ctrl+C] stop Sentum
```

Strategy/symbol changes remain deferred while a position is open. Pausing entries never disables management of an already-open position.

## Data access and performance

Runtime telemetry is read from the in-process `DashboardState` snapshot.

Trades, orders and models are read through `DashboardRepository` only once every two seconds and use independent read-only SQLite connections. This keeps terminal rendering outside the market-data and execution hot paths.

The equity sparkline is a small in-memory sample of displayed paper equity and does not write additional persistence data.

## No fake market depth

Phase 19 does not claim to provide an exchange Level-2 order book. The Market tab labels its bid/ask values as **fill-model bid/ask**, calculated from the configured simulated spread around the latest available position price. A future real order-book panel should be driven by Binance BookTicker/Depth streams.
