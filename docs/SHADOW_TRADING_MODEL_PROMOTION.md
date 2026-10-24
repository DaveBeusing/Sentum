# Shadow trading and model promotion

Sentum supports a controlled model lifecycle from research into progressively more realistic execution environments without exposing an automatic production-trading promotion path.

## Lifecycle

```text
research -> shadow -> paper -> testnet
```

Promotion is linear. Stages cannot be skipped.

## Model definitions

A model definition identifies the strategy, symbol, source experiment, source revision/config hash, risk configuration and promotion policy. Registration validates that the referenced experiment exists, completed successfully and produced a final holdout result.

The promoted candidate must match the strategy parameters and execution assumptions selected by research. Sentum rejects attempts to attach different runtime logic to unrelated holdout evidence. Registration verifies the complete manifest, registered artifact hashes, materialized dataset, symbol and effective risk configuration; see [Model source provenance](MODEL_SOURCE_PROVENANCE.md) for the validation contract and its trust boundary.

## Immutable revisions

Every registered model has two identity fields:

- `definition_revision`: SHA-256 of the normalized complete model document, complete risk document and default-expanded effective risk configuration. The symbol, strategy, source identifiers, risk settings and promotion policy are all included. JSON whitespace and object-key order do not affect the hash; additional fields are included so a future configuration extension cannot inherit old approval silently.
- `model_revision`: a monotonically increasing registration number within that model ID. Changing a definition and later restoring its earlier content still creates a new registration number.

Registration atomically stores the immutable definition/risk snapshot and its freshly verified research evidence. Registering unchanged content preserves its current stage and evidence. An existing research revision created through the low-level registry API without source evidence can be completed by a later validated registration. Changed content starts a new `research` revision; earlier revisions, evidence and promotion events remain in the audit history but do not satisfy the new revision's gates. Stage transitions and their approval events commit together.

Promotion, runtime startup and evidence recording all require the exact current hash and registration number. Editing an external model or risk file therefore cannot inherit approval merely because `model_id` remains unchanged. A changed definition requires matching research provenance and a new progression through the lifecycle.

### Active runtime ownership

Paper and Shadow hold a shared operating-system lease for their model revision. Changed registration and promotion require an exclusive, nonblocking lease; they fail with an instruction to stop the model's runtimes while a runtime still owns that revision. Unchanged registration remains idempotent. The lease works across processes and resolves absolute, relative and symlink aliases of the registry to the same lock identity. A process exit releases its lease automatically.

The runtime executes its validated in-memory strategy and risk snapshot. An on-disk edit does not mutate an already constructed trading engine; Paper revalidates the model and risk document whenever a trader is constructed, and Shadow revalidates before starting. Runtime shutdown retains the lease until workers have stopped and finalization has finished. This protects revision ownership without querying SQLite in the market-event path.

## Shadow trading

Shadow mode consumes live Binance public market data but executes only through `SimulatedExecutionVenue`. It does not require an exchange trading API key and does not place exchange orders.

```bash
./build/sentum_model register config/model.json
./build/sentum_model promote config/model.json shadow I_APPROVE_MODEL_PROMOTION
./build/sentum_model shadow config/model.json
```

Shadow evidence is persisted separately and can be used by the next promotion gate.

Shadow history filenames include the registration number, for example `log/shadow/btc-momentum-v1-r1.sqlite3`. The latest evidence report includes `model_id`, `definition_revision` and `model_revision`; evidence for an earlier registration cannot be attached to a replacement model.

## Promotion policy

Each transition checks configured minimum/maximum criteria such as:

- trade count
- net profit
- profit factor
- win rate
- Sharpe ratio
- maximum drawdown

Promotion also requires explicit operator confirmation. Rejected attempts are written to the audit history.

Independent research validation can be generated for a managed source experiment with `tools/verify_research.py`, but it is currently additional evidence rather than a promotion gate. A verifier `PASS` does not automatically promote a model or change the existing stage policy. Making independent validation mandatory for promotion requires an explicit future policy change after the validation-report contract is considered stable.

## Paper integration

A model that has reached the `paper` stage can be loaded by the interactive Paper runtime through `paper.modelDefinition` in `config/config.json`. The runtime inherits the registered model's strategy, symbol and risk-config path and fails closed if registry state is inconsistent.

The configuration captures the approved revision, and each trader creation verifies that the definition and risk settings still match it. Interactive strategy or symbol changes cannot substitute another candidate while retaining model approval. Stop the model session and select/revalidate the intended candidate before restarting. The Paper account's available equity may reduce the model's approved `max_total_capital` ceiling; a larger account balance never raises that ceiling for a bound model.

## Importing Paper or Testnet evidence

`record-evidence` requires the metrics document to identify its producing model revision and stage explicitly:

```json
{
  "model_id": "btc-momentum-v1",
  "definition_revision": "<SHA-256 from the producing runtime>",
  "model_revision": 1,
  "stage": "paper",
  "started_at_ms": 1710000000000,
  "finished_at_ms": 1710000060000,
  "trades": 50,
  "net_profit": 100.0,
  "max_drawdown": 10.0,
  "profit_factor": 2.0,
  "win_rate": 70.0,
  "expectancy": 2.0,
  "sharpe": 1.5,
  "sortino": 1.8
}
```

Use the identities recorded by the run that produced the metrics. The command rejects missing, obsolete or mismatched identifiers, a stage other than the current stage, negative/overflowing trade counts and invalid or nonfinite metrics. It does not automatically relabel old results with the current revision. Imported evidence remains an explicit operator-provided artifact; revision binding does not establish independent authenticity of manually supplied performance claims.

## Persistence

Model lifecycle state is stored in:

```text
log/models.sqlite3
```

Important tables include:

- `models`
- `model_revisions`
- `stage_evidence`
- `promotion_events`

Shadow trade history is stored separately under `log/shadow/`.

### Existing registry upgrade

The immutable-revision schema is version `1` in SQLite `user_version`. Existing version-`0` registries are upgraded additively: their definitions, stage evidence and promotion history remain available, but their previous approvals have no trustworthy revision binding. Such rows show `revision_bound: false` and cannot authorize execution, promotion or new evidence. Register the model again against verified source research; it then starts at `research` and must pass the normal transitions again. Existing unbound evidence is not silently adopted. A registry with a newer unknown schema is rejected without downgrading its version.

Lease files are stored beside the registry in `<registry-path>.locks/`, with filenames derived from a hash of the model ID. They contain no credentials or trading data; do not delete or replace this directory while model runtimes or lifecycle commands are active.

## Testnet boundary

`testnet` is the highest promotion stage implemented by this lifecycle. Sentum does not define an automatic production-money promotion stage. Testnet execution remains subject to reconciliation, User Data Stream state and kill-switch controls.

## Dashboard and console

The web dashboard and terminal Models view expose lifecycle state read-only. Promotion remains an explicit operator workflow and is not reduced to an unaudited browser or single-key action.
