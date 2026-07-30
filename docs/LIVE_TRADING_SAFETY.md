# Live execution safety

Phase 5 is hard-wired to Binance Spot Testnet (`testnet.binance.vision` and `stream.testnet.binance.vision`). Production endpoints are intentionally absent from the execution client.

## Required environment

```bash
export SENTUM_ENABLE_SPOT_TESTNET=I_UNDERSTAND_TESTNET_ONLY
export SENTUM_BINANCE_TESTNET_API_KEY='...'
export SENTUM_BINANCE_TESTNET_API_SECRET='...'
```

Create an API key that permits Spot trading only. Never enable withdrawals. Sentum neither needs nor implements withdrawal endpoints.

## State authority

A local order request starts as `pending`. A successful REST response moves it only to `acknowledged`. Positions are created or changed only after an exchange event reports `FILLED` with a non-zero exchange order id and executed quantity.

`PARTIALLY_FILLED` remains an order state and does not create a completed local position. Cancellation remains `cancelling` until the User Data Stream or a later reconciliation confirms `CANCELED`/`EXPIRED`.

## Startup

`LiveOrderSession::start()` performs open-order reconciliation before accepting any new order. If reconciliation fails, startup fails closed.

## Kill switch

The kill switch:

1. rejects new orders,
2. attempts to cancel every pending, acknowledged, or partially-filled order,
3. leaves final status unresolved until confirmed by Binance,
4. activates automatically if listen-key keepalive fails,
5. activates during orderly shutdown.

After testnet soak testing, production support must be introduced as a separate reviewed change with an explicit capital cap, production-specific credentials, account/balance reconciliation, and operational runbook. Do not repoint the testnet constants.
