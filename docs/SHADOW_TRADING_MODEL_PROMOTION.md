# Shadow trading and model promotion

Sentum supports a controlled model lifecycle from research into progressively more realistic execution environments without exposing an automatic production-trading promotion path.

## Lifecycle

```text
research -> shadow -> paper -> testnet
```

Promotion is linear. Stages cannot be skipped.

## Model definitions

A model definition identifies the strategy, symbol, source experiment, source revision/config hash, risk configuration and promotion policy. Registration validates that the referenced experiment exists, completed successfully and produced a final holdout result.

The promoted candidate must match the strategy parameters and execution assumptions selected by research. Sentum rejects attempts to attach different runtime logic to unrelated holdout evidence.

## Shadow trading

Shadow mode consumes live Binance public market data but executes only through `SimulatedExecutionVenue`. It does not require an exchange trading API key and does not place exchange orders.

```bash
./build/sentum_model register config/model.json
./build/sentum_model promote config/model.json shadow I_APPROVE_MODEL_PROMOTION
./build/sentum_model shadow config/model.json
```

Shadow evidence is persisted separately and can be used by the next promotion gate.

## Promotion policy

Each transition checks configured minimum/maximum criteria such as:

- trade count
- net profit
- profit factor
- win rate
- Sharpe ratio
- maximum drawdown

Promotion also requires explicit operator confirmation. Rejected attempts are written to the audit history.

## Paper integration

A model that has reached the `paper` stage can be loaded by the interactive Paper runtime through `paper.modelDefinition` in `config/config.json`. The runtime inherits the registered model's strategy, symbol and risk-config path and fails closed if registry state is inconsistent.

## Persistence

Model lifecycle state is stored in:

```text
log/models.sqlite3
```

Important tables include:

- `models`
- `stage_evidence`
- `promotion_events`

Shadow trade history is stored separately under `log/shadow/`.

## Testnet boundary

`testnet` is the highest promotion stage implemented by this lifecycle. Sentum does not define an automatic production-money promotion stage. Testnet execution remains subject to reconciliation, User Data Stream state and kill-switch controls.

## Dashboard and console

The web dashboard and terminal Models view expose lifecycle state read-only. Promotion remains an explicit operator workflow and is not reduced to an unaudited browser or single-key action.
