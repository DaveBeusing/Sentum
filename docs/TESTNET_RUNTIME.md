# Binance Spot Testnet runtime

Sentum supports Binance Spot Testnet execution with exchange-confirmed order state, startup reconciliation and a persistent kill switch. Production Binance endpoints are intentionally not used by this runtime.

## Start

```bash
export SENTUM_ENABLE_SPOT_TESTNET=I_UNDERSTAND_TESTNET_ONLY
export SENTUM_BINANCE_TESTNET_API_KEY='...'
export SENTUM_BINANCE_TESTNET_API_SECRET='...'
./sentum testnet BTCUSDT
```

Use a Testnet API key with only the permissions required for Spot Testnet trading. Withdrawal permissions are neither required nor appropriate.

## Order-state authority

Supported order states are:

```text
pending
acknowledged
partially filled
filled
cancelling
cancelled
rejected
```

A strategy signal or REST placement acknowledgement does not create an executed local position. Confirmed position state changes only after exchange-confirmed fills or reconciliation against exchange state.

### Callback and submission safety

The strategy reserves an order identity while holding its state mutex, then releases
that mutex before calling the venue. Synchronous Pending, ACK, rejection and fill
callbacks can therefore update the same strategy without deadlocking the price
worker. A separate submission reservation lasts until the venue call returns,
including when a terminal callback has already cleared the active order. Competing
price ticks cannot submit during that interval. Only matching symbol/order callbacks
change that reservation; a returned ACK is not processed as a second fill event.

`OrderManager` copies order updates under its own mutex and invokes observers after
releasing it. Observers may inspect orders or trigger cancellation and the kill
switch. An observer exception is propagated as an observer failure, rather than
being recorded as an exchange rejection. A kill triggered by a failed Pending
audit write prevents the reserved order from subsequently reaching the exchange.
Evidence-write failure latches one kill request even when cancellation itself emits
another callback. A submission exception that escapes the venue blocks further
strategy submissions while retaining the unresolved order identity.

The `testnet_callback_safety` regression uses the real runtime, real order manager,
local SQLite failure triggers and synchronous fixture venues. It covers entries,
exits, competing ticks, reentrant observers, persistence failures and bounded
shutdown without exchange credentials. This qualifies the callback boundary;
subscription migration, authenticated readiness, inventory reconciliation and the
remaining cumulative-fill/order-state contracts require their separate corrections.

## Startup and recovery

The runtime reconciles open orders and balances before enabling submissions, then starts the Binance User Data Stream. Stream interruption or listen-key keepalive failure disables new submissions, activates recovery controls, rebuilds the stream and requires reconciliation again.

Ambiguous recovery remains blocked rather than guessing local state. A rebuilt User Data Stream does not clear a kill switch automatically; explicit reconciled resume is required, and unresolved open orders continue to block that transition.

See [ACCOUNT_RECONCILIATION.md](ACCOUNT_RECONCILIATION.md) for the complete recovery model.

## Persistence and observability

Order transitions are appended to `order_events` in the runtime SQLite database. Runtime and recovery activity is additionally recorded in operational audit tables.

`log/status.json` provides machine-readable Testnet state including symbol, stream health, reconciliation status, kill-switch state, latest price, signal/risk information, order state and confirmed position quantity.

The terminal console and web dashboard consume this operational state for read-only visibility.

## Replay verification

Replay writes deterministic trade history and metrics that can be checked with the independent verification tool:

```bash
python3 tools/verify_metrics.py log/replay.sqlite3 log/replay_metrics.json
```

Testnet behavior is qualified without live credentials through the deterministic scenarios in [RUNTIME_QUALIFICATION.md](RUNTIME_QUALIFICATION.md). Those scenarios cover User Data Stream interruption, listen-key failure, partial fills, restart reconciliation, balance mismatch, unresolved orders and kill-switch recovery while preserving the production order/reconciliation state machines.
