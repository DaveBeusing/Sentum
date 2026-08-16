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

## Startup and recovery

The runtime reconciles open orders and balances before enabling submissions, then starts the Binance User Data Stream. Stream interruption or listen-key keepalive failure disables new submissions, activates recovery controls, rebuilds the stream and requires reconciliation again.

Ambiguous recovery remains blocked rather than guessing local state.

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

Testnet behavior should be validated under partial fills, reconnects, process restart, balance mismatches and unresolved-order scenarios before it is treated as operationally reliable.
