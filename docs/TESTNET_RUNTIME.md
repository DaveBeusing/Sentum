# Binance Spot Testnet runtime

Sentum provides a Binance Spot Testnet order boundary with authenticated user-data subscriptions, exchange-confirmed cumulative execution, durable order identity and a persistent kill switch. REST order routing and strategy market prices use explicit Testnet endpoints. Admission requires a complete account/execution proof and successful restoration of the strategy's confirmed position and protective policy.

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
outcomes, partial inventory and restart-safe latches. Session recovery tests exercise
the authenticated stream barrier, account proof and durable adoption together.

## Startup and recovery

The session loads its durable latch and canonical order ownership before subscribing. A correlated successful acknowledgement of `userDataStream.subscribe.signature` is required before the stream is healthy. Worker existence alone grants no readiness. The subscribed stream remains active while REST evidence is collected, and received-event and connection generations protect final publication from concurrent changes.

Persisted unresolved requests are queried using the original identity. Bounded, paginated account trades provide the execution and commission evidence for reconstructing inventory, entry time and cooldown history. Complete account and open-order schemas are required; missing fields never imply a flat account. Verified replacements and replay commit atomically before the runtime restores its owned position. Unowned events or holdings, unknown outcomes, unresolved orders and incomplete or changing evidence keep submissions blocked.

The runtime shares fixed stop, trailing stop, take profit and maximum-holding decisions with Paper/replay. It persists protective policy checkpoints before publishing tighter protection. A compatible checkpoint is required when restoring positive inventory; missing, corrupt or mismatched protection blocks admission. A reconnect or restart cannot lower a previously saved trailing stop. See [Trailing-stop risk policy](TRAILING_STOP_RISK.md).

Ambiguous recovery remains blocked rather than guessing local state. A rebuilt User Data Stream does not clear a kill switch automatically; explicit reconciled resume is required, and unresolved open orders continue to block that transition.

See [ACCOUNT_RECONCILIATION.md](ACCOUNT_RECONCILIATION.md) for the complete recovery model.

## Market data and submission rules

The Testnet strategy receives trades from `wss://stream.testnet.binance.vision/ws/<symbol>@trade`. Its environment and endpoint appear in runtime status. The Paper/public feed retains its public-market endpoint. A Testnet runtime rejects a venue with incompatible market provenance.

Current symbol rules are loaded before admission and refreshed before each submission. Symbol status, Spot availability, MARKET support and the account's actual permission set must agree. Quantity normalization intersects active `LOT_SIZE`, `MARKET_LOT_SIZE`, base precision and local step constraints using exact decimal grids. Disabled zero fields do not erase another active filter. Normalization never increases approved size. The canonical decimal quantity is sent unchanged to REST.

Notional checks use current Testnet reference-price evidence when available, otherwise the filter's exact average-price window or last-price fallback. Stale, missing or inconsistent evidence blocks submission. Local capital, risk, quantity and notional limits still apply after normalization; exchange metadata never grants extra risk budget. See [Exchange order validation](EXCHANGE_ORDER_VALIDATION.md).

## Persistence and observability

Order transitions retain their `order_events` audit history. The execution boundary additionally stores canonical accepted order snapshots, replay history, reconciliation proof, position-policy checkpoints and kill-switch state under its credential namespace before publishing state to observers. Execution schema version 2 adds tables without discarding existing audit data or changing the shared SQLite `user_version`. The durable session guard and exclusive execution lease preserve fail-closed behavior through process interruption and storage failure. See [TESTNET_ORDER_STATE.md](TESTNET_ORDER_STATE.md) for migration and resume requirements.

`log/status.json` provides machine-readable Testnet state including symbol, market environment and endpoint, authenticated user-stream health, reconciliation status, kill-switch state, latest price, signal/risk information, order state and confirmed position quantity. A disconnect, subscription termination or authentication failure immediately closes admission and latches the kill switch. Rebuilding a healthy subscription initiates recovery but never clears that latch automatically.

The terminal console and web dashboard consume this operational state for read-only visibility.

## Replay verification

Replay writes deterministic trade history and metrics that can be checked with the independent verification tool:

```bash
python3 tools/verify_metrics.py log/replay.sqlite3 log/replay_metrics.json
```

## Qualification boundaries

Order and recovery boundaries are qualified without live credentials through the deterministic scenarios in [RUNTIME_QUALIFICATION.md](RUNTIME_QUALIFICATION.md). The authenticated subscription regression uses actual loopback TLS/WebSocket connections, acknowledgement/error frames, timeouts, disconnects, ping/pong and restart. Integrated session fixtures cover fills during outages, account/schema failures, ownership, atomic SQLite adoption, changing stream generations and explicit resume. Policy tests compare the real replay and Testnet runtime decisions and verify preserved protection across recovery.

These tests establish repository behavior against controlled evidence. They do not constitute a credentialed Binance acceptance run, predict unobserved prices during downtime, or authorize production order routing. Existing unexplained Testnet seed inventory, legacy unbound history, unsupported history volume or missing position protection remains blocked. See [Authenticated User Data Stream](TESTNET_USER_DATA_STREAM.md) for the subscription contract and official protocol references.
