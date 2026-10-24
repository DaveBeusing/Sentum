# Account reconciliation and recovery

Sentum treats the exchange as the authority for executed Testnet state. Local intent, REST acknowledgements and strategy signals do not create a confirmed position by themselves.

`AccountReconciler` provides a comparison of local position quantity with Binance Spot Testnet balances and open orders. Its complete integration with persisted fills, commissions and the strategy position remains tracked in [#68](https://github.com/DaveBeusing/Sentum/issues/68). The supported session currently implements the durable order-recovery boundary described here and blocks historical inventory it cannot restore safely.

## Exchange rules

`ExchangeRules` loads symbol filters from Binance exchange metadata, including quantity increments, minimum/maximum quantity, notional limits and precision. Local risk controls may be stricter than exchange rules but cannot weaken them.

## Implemented order-recovery sequence

The order/session boundary follows a fail-closed sequence:

1. Acquire the credential-scoped execution lease and load the persisted latch/session guard.
2. Restore canonical orders and replay accepted execution history idempotently.
3. Query unresolved orders by their original client order IDs and inspect exchange open orders.
4. Keep unresolved outcomes, incomplete legacy evidence and un-restorable inventory blocked.
5. Start/recover the stream through the existing transport boundary; this does not clear a latch.

If reconciliation, storage, stream recovery or exchange-state verification fails, the kill switch remains active and submissions stay blocked. Authoritative order resolution is necessary for resume; it is not proof of complete account reconciliation.

## Runtime recovery

An observed stream interruption or keepalive failure disables new submissions and latches the kill switch. Rebuilding the stream does not authorize trading. Unknown submission outcomes are polled using the same original identity, and no recovery path retries placement to discover an outcome. The existing worker-health abstraction and retired listen-key transport require the separate corrections tracked in [#66](https://github.com/DaveBeusing/Sentum/issues/66) and [#67](https://github.com/DaveBeusing/Sentum/issues/67).

Explicit resume uses `LiveOrderSession::resume_after_reconcile` with the required confirmation string. The clear must be durably audited before the in-memory latch is released. Failed writes, unresolved orders and incomplete recovery leave it latched. The CLI has no latch-clearing override.

Clean shutdown also persists the latch. Restarting after a normal stop therefore
requires explicit reconciled resume. Preserve the configured journal when moving
the runtime: historical evidence in another database path is not discovered
automatically.

## Persistent audit data

The durable order repository stores accepted snapshots, ordered history, kill-switch audit and a process-session guard alongside existing audit tables. Legacy audit-only rows are preserved but cannot prove complete ownership or commission history. See [TESTNET_ORDER_STATE.md](TESTNET_ORDER_STATE.md) for the authoritative contract and migration boundary.

Existing operational evidence includes:

- `order_events`
- `runtime_events`
- `reconciliation_runs`
- `kill_switch_events`

`log/status.json` provides machine-readable Testnet runtime state including stream health, reconciliation status, kill-switch state, confirmed position and recent execution information.

## Execution boundary

`IExecutionVenue` is the shared order boundary. `BinanceTestnetExecutionVenue` operates against Binance Spot Testnet, while `SimulatedExecutionVenue` implements the same request/update model for Paper, replay, shadow and research workflows.

## Validation

Before relying on Testnet recovery behavior, validate balance mismatches, unresolved orders, partial fills, process restart, User Data Stream interruption, dynamic symbol filters and kill-switch recovery under Release and sanitizer builds.
