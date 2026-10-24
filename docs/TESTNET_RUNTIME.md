# Binance Spot Testnet runtime

Sentum provides a Binance Spot Testnet order boundary with exchange-confirmed cumulative execution, durable order identity and a persistent kill switch. REST order routing is restricted to the Testnet endpoint. Complete Testnet operation still depends on the outstanding subscription, authenticated-health, account-reconciliation and market-feed corrections described below.

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
unknown
```

A strategy signal or REST placement acknowledgement does not create an executed local position. Confirmed position state changes by exchange-confirmed execution deltas, including partial fills. Cancellation retains already executed inventory, and duplicate reports cannot apply the same quantity or commission twice.

An uncertain submission is retained as `unknown`, queried using its original client order ID, and blocks further order entry. The complete identity, persistence and accounting contract is documented in [TESTNET_ORDER_STATE.md](TESTNET_ORDER_STATE.md).

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
shutdown without exchange credentials. The order-state, durability and execution-delta
regressions additionally qualify monotonic state, cancellation identity, unknown
outcomes, partial inventory and restart-safe latches. Subscription migration,
authenticated readiness and complete account/runtime reconciliation remain separate
requirements.

## Startup and recovery

The session loads its durable latch and canonical order history before order reconciliation. Persisted unresolved requests are queried using the original identity. Unknown outcomes, unresolved orders, incomplete historical evidence and recovered inventory that the strategy cannot safely restore keep submissions blocked. This is the order-recovery foundation; full account-balance and strategy-position reconciliation remains open in [#68](https://github.com/DaveBeusing/Sentum/issues/68).

Ambiguous recovery remains blocked rather than guessing local state. A rebuilt User Data Stream does not clear a kill switch automatically; explicit reconciled resume is required, and unresolved open orders continue to block that transition.

See [ACCOUNT_RECONCILIATION.md](ACCOUNT_RECONCILIATION.md) for the complete recovery model.

### Outstanding end-to-end requirements

The current listen-key subscription transport requires migration in [#66](https://github.com/DaveBeusing/Sentum/issues/66). Worker lifecycle is not yet a sufficient authenticated-subscription health signal; [#67](https://github.com/DaveBeusing/Sentum/issues/67) tracks that readiness correction. Testnet strategy prices still need explicit Testnet market-feed selection in [#80](https://github.com/DaveBeusing/Sentum/issues/80). Passing local order fixtures does not establish complete Testnet readiness.

## Persistence and observability

Order transitions retain their `order_events` audit history. The execution boundary additionally stores canonical accepted order snapshots, replay history and kill-switch state under its credential namespace before publishing state to observers. The durable session guard and exclusive execution lease preserve fail-closed behavior through process interruption and storage failure. See [TESTNET_ORDER_STATE.md](TESTNET_ORDER_STATE.md) for migration and resume requirements.

`log/status.json` provides machine-readable Testnet state including symbol, stream health, reconciliation status, kill-switch state, latest price, signal/risk information, order state and confirmed position quantity.

The terminal console and web dashboard consume this operational state for read-only visibility.

## Replay verification

Replay writes deterministic trade history and metrics that can be checked with the independent verification tool:

```bash
python3 tools/verify_metrics.py log/replay.sqlite3 log/replay_metrics.json
```

Order and recovery boundaries are qualified without live credentials through the deterministic scenarios in [RUNTIME_QUALIFICATION.md](RUNTIME_QUALIFICATION.md). Stream-interruption fixtures exercise injected stream state; the balance-mismatch scenario exercises `AccountReconciler` independently. These tests do not substitute for the outstanding real subscription and integrated account-recovery work.
