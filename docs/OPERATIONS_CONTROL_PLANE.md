# Operations Control Plane & Governance

## Purpose

The Sentum operations control plane aggregates operational evidence and classifies maintenance/recovery actions. It does not become a new trading, Risk or Execution authority.

## Authority boundary

The control plane may coordinate diagnostics, maintenance and governed recovery workflows. It must not synthesize fills, mutate Risk or Execution truth, override exchange-confirmed execution truth or silently resume entries.

Trading-state authority remains in the existing Strategy/Risk/Execution path. Exchange-confirmed fills remain authoritative for execution truth.

## Governance classes

### AUTOMATED

Low-risk, non-authoritative operations may run without operator approval when upstream resilience evidence is PASS and the action is explicitly allow-listed.

Examples:
- collect diagnostics;
- rotate log sink;
- refresh non-authoritative caches;
- restart a non-authoritative worker.

### APPROVAL_REQUIRED

Actions that can alter operational acceptance, resume trading or cross a safety boundary require explicit operator approval and an audit record.

Examples:
- open an incident from a governed proposal;
- resume entries;
- clear kill switch;
- promote a recovery candidate;
- accept reconciliation;
- enter or exit maintenance mode.

### FORBIDDEN

The control plane must reject actions that would create or override authoritative trading truth.

Examples:
- synthesize fills;
- mutate Risk state;
- mutate Execution state;
- override exchange-confirmed truth.

## Audit contract

Every governed action record must contain at least:
- action;
- classification;
- actor;
- reason;
- Git SHA;
- source workflow run identifier;
- UTC timestamp.

Approval-required actions additionally require an explicit approval identity and decision record in a real target environment.

## Aggregated operational state

The control-plane gate consumes the resilience guardrail evidence for the same Git commit and environment class. A PASS result means repository policy and classification rules are internally consistent. It does not mean a production action was executed.

The emitted governance state is:
- `CONTROLLED` when policy, upstream evidence and classifications are valid;
- `BLOCKED` when evidence is missing/mismatched or a governance invariant is violated.

## Governed incident lifecycle

The operations control plane now persists the `OPEN_INCIDENT` request, decision, incident state, recovery state and transition history in the configured runtime SQLite database.

Terminal notification failure evidence may submit an idempotent approval-required request. It cannot approve the request or open the incident. Approval and denial require an explicit operator identity and reason.

Acknowledgement remains distinct from recovery and resolution. Recovery requires prior acknowledgement plus reconciliation evidence. Resolution and closure never clear a kill switch, resume entries, alter Risk/Execution state or create exchange truth.

See `GOVERNED_INCIDENT_LIFECYCLE.md` for the state and operator-command contract.

## Fail-closed behavior

Unknown or unclassified actions must not be executed automatically. Missing approval, mismatched evidence, missing audit fields or forbidden action requests must stop the operation and require operator review.

## Evidence boundary

CI uses `environment_class = ci_rehearsal`. CI evidence proves policy behavior only. Production governance evidence must come from the target environment and preserve the actual actor, approval and action outcome.

## Non-goals

This control plane does not:
- enable live trading;
- clear safety interlocks automatically;
- replace reconciliation;
- replace exchange-confirmed execution truth;
- mutate Strategy, Risk or Execution state directly;
- create a second lifecycle authority.
