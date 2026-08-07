# Sentum Shadow Trading & Model Promotion

Phase 15 adds an explicit model lifecycle between research and exchange execution. The pipeline is intentionally fail-closed and does not introduce production-money execution.

## Lifecycle

```text
completed research experiment
        |
        v
     research
        |
        | manual promotion + quality gates
        v
      shadow
        |
        | live Binance market data, simulated fills only
        | manual promotion + quality gates
        v
       paper
        |
        | persisted stage evidence
        | manual promotion + quality gates
        v
      testnet

production: deliberately unsupported
```

No stage can be skipped and no promotion is automatic.

## Model definition

Copy `config/model.example.json` and fill the immutable source provenance from a completed Phase-13 experiment:

- `source_experiment_run`
- `source_git_commit`
- `source_config_sha256`
- strategy definition
- risk configuration
- promotion thresholds

Registration verifies the source run in `log/experiments.sqlite3`. The source must be `completed`, contain a final Holdout result, and match the supplied Git/config provenance when those fields are provided.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

The Phase-15 CLI is:

```text
build/sentum_model
```

## Register candidate

```bash
./build/sentum_model register config/model.json
```

Registration stores the candidate in:

```text
log/models.sqlite3
```

and imports the final Holdout metrics of the source experiment as `research` evidence.

## Promote to shadow

```bash
./build/sentum_model promote \
  config/model.json shadow I_APPROVE_MODEL_PROMOTION
```

Promotion is rejected unless the Research evidence satisfies all configured gates.

## Shadow trading

```bash
./build/sentum_model shadow config/model.json
```

Shadow mode consumes real Binance market prices through the existing public WebSocket client but uses the normal Sentum `TradeEngine`, `RiskManager`, strategy framework and simulated execution path. It submits no REST order and uses no API key.

Shadow trades are persisted under:

```text
log/shadow/<model-id>.sqlite3
```

On clean shutdown the completed trade set is converted into stage evidence and stored in the model registry. A JSON snapshot is written to:

```text
log/shadow/<model-id>-latest.json
```

A single completed shadow session is the evidence unit used by the promotion gate. This avoids statistically invalid averaging of independent Sharpe/Drawdown metrics across sessions.

## Promotion gates

Configured in the model definition:

```json
{
  "promotion_policy": {
    "min_trades": 30,
    "min_net_profit": 0.0,
    "min_profit_factor": 1.10,
    "min_win_rate": 45.0,
    "min_sharpe": 0.50,
    "max_drawdown": 250.0
  }
}
```

Every transition requires all gates and the exact operator confirmation token:

```text
I_APPROVE_MODEL_PROMOTION
```

The token is intentionally unsuitable for unattended/background promotion.

## Paper and Testnet evidence

Phase 15 provides a strict registry boundary for downstream runtime evidence:

```bash
./build/sentum_model record-evidence config/model.json paper log/paper-evidence.json
./build/sentum_model promote config/model.json testnet I_APPROVE_MODEL_PROMOTION
```

Evidence can only be recorded for the model's current stage. This prevents a future-stage metrics file from being injected before the model actually reaches that stage.

The evidence JSON accepts:

```json
{
  "started_at_ms": 0,
  "finished_at_ms": 0,
  "trades": 50,
  "net_profit": 120.0,
  "max_drawdown": 45.0,
  "profit_factor": 1.4,
  "win_rate": 54.0,
  "expectancy": 2.4,
  "sharpe": 1.1,
  "sortino": 1.5
}
```

A future runtime integration should generate these evidence files directly from the persistent Paper/Testnet execution ledger. Phase 15 deliberately does not infer or fabricate exchange evidence.

## Registry audit trail

`log/models.sqlite3` contains:

```text
models
stage_evidence
promotion_events
```

Every attempted promotion records:

- model ID
- old and requested stage
- approved/rejected result
- reason
- operator confirmation
- timestamp

Failed gates therefore remain auditable.

## Safety properties

- research provenance must resolve to a completed experiment
- live Shadow uses public market data only
- Shadow cannot submit exchange orders
- stages cannot be skipped
- promotion is never automatic
- all promotion attempts are persistent
- Paper/Testnet evidence must correspond to the current stage
- no `production` stage exists in the Phase-15 state machine
- Testnet remains the highest promotable stage
