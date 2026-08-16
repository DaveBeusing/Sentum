# Execution safety

Sentum is designed around simulated execution, Binance Spot Testnet and explicit safety boundaries. Production Binance order routing and withdrawal functionality are not part of the supported runtime described by this repository.

## Paper and research

Paper, replay, shadow and research use `SimulatedExecutionVenue`. Paper consumes live public market data but does not submit exchange orders. Simulated fills account for configured fees, spread and slippage.

Paper trading uses a persistent simulated account rather than the authenticated Binance account balance.

## Testnet

Binance Spot Testnet uses exchange-confirmed order state. A local signal or REST acknowledgement is never treated as an executed position without a confirmed fill or successful reconciliation.

New Testnet submissions remain blocked until startup reconciliation and the User Data Stream are healthy. Stream failures, unresolved orders or balance mismatches keep trading blocked and can latch the kill switch.

## Model promotion

Model promotion is intentionally staged:

```text
research -> shadow -> paper -> testnet
```

Each transition requires evidence that passes configured policy gates plus explicit operator confirmation. The lifecycle does not contain an automatic production-money stage.

## Credentials

Do not commit API credentials. Testnet keys should have only the permissions required for Spot Testnet trading and should not have withdrawal rights.

## Dashboard and terminal UI

The web dashboard remains read-only. The terminal Paper controls can select strategy/symbol, pause new entries and request a simulated close, but they do not enable production exchange execution.

## Operational expectations

Before relying on Sentum even in Testnet, validate long-running Paper behavior, queue/drop limits, sanitizer builds, User Data Stream interruption, partial fills, restart reconciliation, balance mismatches and kill-switch recovery.

Sentum remains experimental trading-system software. Historical or simulated performance must not be interpreted as a guarantee of future results.
