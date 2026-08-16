# Web dashboard

Sentum includes a lightweight read-only web dashboard served directly by the C++ runtime. Paper and Testnet modes start it automatically; it can also run standalone against persisted Sentum data.

## Start

```bash
./sentum dashboard
```

Dashboard bind settings are controlled by `config/config.json`:

```json
{
  "dashboardHost": "127.0.0.1",
  "dashboardPort": 8080
}
```

Use `127.0.0.1` for local-only access. Binding to `0.0.0.0` exposes the service on available network interfaces and should only be used when network/firewall controls are appropriate.

A one-off port override remains available:

```bash
./sentum dashboard --dashboard-port 8090
```

## Runtime and research views

The dashboard combines current runtime state with persisted history and research artifacts. It exposes:

- Paper/Testnet health and service status
- account/equity and P/L information
- active symbol and position state
- scanner rankings
- trade and order history
- runtime performance and persistence health
- replay/backtest metrics
- experiment history and research comparison
- holdout equity/drawdown visualization
- parameter landscapes and robustness statistics
- registered model lifecycle state

## API

The browser surface is GET-only. Important endpoints include:

```text
GET /api/health
GET /api/status
GET /api/trades?limit=100
GET /api/orders?limit=100
GET /api/equity?limit=500
GET /api/replay
GET /api/research
GET /api/experiments?limit=200
GET /api/experiment?run_id=<run-id>
GET /api/experiment/trials?run_id=<run-id>&limit=10000
GET /api/models
GET /api/model?model_id=<model-id>
```

Non-GET requests are rejected. The web dashboard does not expose order placement, cancellation, model promotion, configuration writes, credential access, kill-switch reset or production-trading activation.

## Data sources

Live process state comes from `DashboardState`. Historical trades/orders are read through independent read-only SQLite connections. Testnet operational state can additionally be read from `log/status.json`. Research views use `log/experiments.sqlite3` and immutable experiment artifacts.

The dashboard remains outside the market-data and execution hot path. Browser reads do not use the SQLite writer connection.

## Security

The dashboard is not an authenticated remote-control plane. If it is bound beyond loopback, protect it with host/network controls and treat all exposed runtime and research information as operational data.
