# Account reconciliation

This phase adds account and balance reconciliation, operational event persistence, dynamic Binance symbol rules, shared execution venues, richer runtime metrics, and a controlled recovery flow.

`AccountReconciler` compares confirmed local quantity with Binance Spot Testnet balances and records reconciliation results. Any mismatch leaves order submission disabled.

`ExchangeRules` reads quantity, notional, and precision filters from `/api/v3/exchangeInfo`. Local risk limits may be stricter but cannot weaken exchange requirements.

Operational audit tables include `runtime_events`, `reconciliation_runs`, `kill_switch_events`, and `order_events`.

`IExecutionVenue` is the common order boundary. `BinanceTestnetExecutionVenue` handles exchange-confirmed orders, while `SimulatedExecutionVenue` provides the same request/update contract for paper and replay paths.

Recovery requires an explicit operator acknowledgement, a successful reconciliation, a rebuilt User Data Stream, and no unresolved orders. A failed check keeps the kill switch active.

`RuntimeMetrics` exposes event ages, REST errors, reconnects, reconciliation counts, order counters, database failures, uptime, and kill-switch reason for inclusion in `log/status.json`.

Before merge, validate Release and sanitizer builds, balance mismatch handling, unresolved orders, controlled recovery, dynamic filters across several symbols, and persistence behavior after restart.
