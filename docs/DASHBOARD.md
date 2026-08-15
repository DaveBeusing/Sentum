# Sentum Dashboard

Phase 9 adds a lightweight read-only web dashboard directly to the Sentum C++ runtime.

## Security model

The server binds only to loopback:

```text
127.0.0.1
```

It intentionally exposes no order placement, cancel, kill-switch reset, credential, configuration-write, or live-trading enable endpoints. Trading control remains outside the browser UI.

The default port is `8080` and can be changed with:

```bash
export SENTUM_DASHBOARD_PORT=8090
```

## Runtime modes

Paper mode starts the dashboard automatically:

```bash
./sentum paper
```

Testnet mode also starts it automatically:

```bash
./sentum testnet BTCUSDT
```

The dashboard can be run independently against persisted Sentum data:

```bash
./sentum dashboard
```

Open:

```text
http://127.0.0.1:8080
```

Replay writes `log/replay_metrics.json`; standalone dashboard mode can display the most recent replay metrics.

## Views

The initial dashboard provides:

- runtime mode and health
- quote balance
- net P&L, completed trades, and win rate
- current trading symbol
- market persistence drop rate
- market-data, user-stream, reconciliation, kill-switch, collector, and scanner health
- current scanner top ranking
- cumulative realized-equity curve
- recent completed trades
- recent exchange/order-state events
- latest replay/backtest metrics

The browser refreshes read-only API data every two seconds.

## API

All endpoints are GET-only:

```text
GET /api/health
GET /api/status
GET /api/trades?limit=100
GET /api/orders?limit=100
GET /api/equity?limit=500
GET /api/replay
```

Non-GET requests return HTTP 405.

## Data sources

Live in-process paper state is published through `DashboardState`.

Testnet state additionally reuses the existing atomic `log/status.json` runtime status written by the testnet execution layer.

Persistent history is read using a separate read-only SQLite connection from:

```text
log/klines.sqlite3
```

This keeps dashboard reads outside the market-data and execution hot paths. SQLite busy timeout is limited to one second and unavailable/missing tables produce empty dashboard datasets instead of stopping the trading runtime.

## Architecture

```text
Sentum Core
   |-- DashboardState (in-process snapshot)
   |-- status.json (testnet runtime state)
   |-- SQLite WAL (trades/order events)
   |-- replay_metrics.json
   |
   +--> DashboardServer / Boost.Beast
           |
           +--> embedded HTML/CSS/JS
           +--> read-only JSON API
```

The dashboard has no CDN or external JavaScript dependency, so it remains usable on an isolated trading host.

## Future dashboard work

Good follow-up additions include WebSocket/SSE push telemetry, richer position/risk cards, latency histograms, strategy comparison, replay-run history, and Prometheus-compatible metrics. Browser-side trading controls should remain gated behind a separate security design rather than being added casually to this read-only server.
