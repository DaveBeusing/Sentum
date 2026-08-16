# Account reconciliation and recovery

Sentum treats the exchange as the authority for executed Testnet state. Local intent, REST acknowledgements and strategy signals do not create a confirmed position by themselves.

`AccountReconciler` compares locally confirmed position quantity with Binance Spot Testnet balances and open orders. Any unresolved mismatch keeps order submission disabled.

## Exchange rules

`ExchangeRules` loads symbol filters from Binance exchange metadata, including quantity increments, minimum/maximum quantity, notional limits and precision. Local risk controls may be stricter than exchange rules but cannot weaken them.

## Startup sequence

Testnet startup follows a fail-closed sequence:

1. Load local persisted state.
2. Reconcile open exchange orders and balances.
3. Create and start the Binance User Data Stream.
4. Confirm reconciliation completed without unresolved state.
5. Enable new order submission only after the checks pass.

If reconciliation, stream recovery or exchange-state verification fails, the kill switch remains active and submissions stay blocked.

## Runtime recovery

User Data Stream interruption or keepalive failure disables new submissions. Sentum rebuilds the stream and reconciles exchange state again before allowing further activity. Ambiguous or unresolved order state is not guessed locally.

## Persistent audit data

Operational state is persisted in SQLite tables including:

- `order_events`
- `runtime_events`
- `reconciliation_runs`
- `kill_switch_events`

`log/status.json` provides machine-readable Testnet runtime state including stream health, reconciliation status, kill-switch state, confirmed position and recent execution information.

## Execution boundary

`IExecutionVenue` is the shared order boundary. `BinanceTestnetExecutionVenue` operates against Binance Spot Testnet, while `SimulatedExecutionVenue` implements the same request/update model for Paper, replay, shadow and research workflows.

## Validation

Before relying on Testnet recovery behavior, validate balance mismatches, unresolved orders, partial fills, process restart, User Data Stream interruption, dynamic symbol filters and kill-switch recovery under Release and sanitizer builds.
