# Testnet runtime

Sentum exposes three explicit modes:

```bash
./client --paper
./client --replay data/btcusdt.csv BTCUSDT
./client --testnet BTCUSDT
```

The Testnet mode requires:

```bash
export SENTUM_ENABLE_SPOT_TESTNET=I_UNDERSTAND_TESTNET_ONLY
export SENTUM_BINANCE_TESTNET_API_KEY='...'
export SENTUM_BINANCE_TESTNET_API_SECRET='...'
```

The API key must permit Spot trading only and must not have withdrawal permission.

## Execution authority

Strategy signals and REST acknowledgements do not create positions. `TestnetStrategyRuntime` changes confirmed position state only after an exchange `FILLED` update with a non-zero exchange order ID and positive executed quantity.

## Startup and recovery

1. Reconcile all open orders.
2. Create a User Data Stream listen key.
3. Start the stream.
4. Enable submissions only after reconciliation completes.
5. Keep the listen key alive.
6. On keepalive failure, disable submissions, activate the kill switch, rebuild the stream, and reconcile again.
7. A kill switch remains latched until process restart and operator review.

## Persistence

Order transitions are appended to `order_events` in `log/klines.sqlite3`. Each row includes client/exchange IDs, state, quantities, fill price, source, exchange timestamp, and local timestamp.

## Observability

Testnet mode atomically updates `log/status.json`. The file reports mode, symbol, stream state, reconciliation status, kill-switch state, latest price, last signal/risk decision, order state, confirmed position quantity, and exit reason.

## Independent replay metric verification

A replay writes:

- `log/replay.sqlite3`
- `log/replay_metrics.json`

Validate the C++ metrics with the independent Python implementation:

```bash
python3 tools/verify_metrics.py log/replay.sqlite3 log/replay_metrics.json
```
