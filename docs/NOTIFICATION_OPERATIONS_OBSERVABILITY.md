# Notification Operations Observability

AP-21 introduces a read-only operational projection over the append-only notification delivery evidence produced by AP-20.

## Purpose

The projection answers operational questions without calling providers and without mutating alert, incident, risk or execution state:

- how many notification deliveries are currently pending or dispatched;
- how many are delivered or failed;
- how many failures remain retryable;
- how many failures are terminal;
- how large the current notification backlog is;
- which channels currently carry active, delivered or failed work;
- whether the evidence indicates normal operation, attention or an incident candidate.

## Durable current-state reduction

Delivery evidence is append-only and persisted in SQLite through `NotificationDeliveryEvidenceRepository`. Operational counts use only the latest persistent evidence record for each generation-safe delivery `dedup_key`.

Persistent sequence, not wall-clock timestamp, defines transition order. Historical transitions such as `PENDING -> DISPATCHED -> DELIVERED` remain in the evidence log but count as one current delivery whose latest state is `DELIVERED`.

The default durable latest-state query is bounded to 1024 dedup keys with a hard repository ceiling of 4096 records. A truncated current-state set is not accepted as partial truth and produces `UNAVAILABLE`.

The cross-surface operations endpoint opens delivery evidence read-only from the configured runtime database and replaces volatile notification evidence with the durable latest-state set before reduction. Missing, unreadable or corrupt persistence fails closed. The endpoint never loads the full notification history during a normal refresh.

Channel rows are sorted deterministically.

## Dispatch runtime diagnostics\n\nThe same notification operations object exposes a `dispatch_runtime` diagnostic projection. It reports runtime status, queue depth, active dispatches, cumulative delivered/retriable/terminal outcomes, provider latency, timeout count and queue rejection count. These counters are diagnostics only; durable latest-state delivery evidence remains authoritative for health and incident-candidate reduction.\n\nAll runtime metrics retain `execution_authorized = false`.\n\n## Health states

The initial code-owned projection exposes:

- `NO_ACTIVITY` — valid evidence source with no current delivery records;
- `HEALTHY` — current records contain no retryable or terminal delivery failures and backlog is below the attention threshold;
- `ATTENTION` — retryable delivery failures exist or the current backlog reaches the configured presentation threshold;
- `INCIDENT_CANDIDATE` — terminal delivery failure evidence reaches the incident-candidate threshold;
- `UNAVAILABLE` — delivery evidence is not available to the projection.

The default attention threshold is 8 current backlog items. The default terminal-failure incident-candidate threshold is 1.

These thresholds classify operational presentation only. They do not create or resolve incidents.

## Incident integration boundary

`incident_signal` is advisory evidence only. `INCIDENT_CANDIDATE` means that incident-management policy should evaluate the delivery failure; it does not mutate `incident_state` and does not authorize incident creation.

The projection always carries:

- `incident_authorized = false`;
- `execution_authorized = false`.

Notification delivery health cannot clear a kill switch, resume entries, approve governed actions, mutate Risk or Execution state, synthesize fills or override exchange-confirmed execution truth.

## Regression coverage

`notification_operations_observability_tests` verifies:

- empty but available evidence is `NO_ACTIVITY`;
- append-only history is reduced to the latest state per dedup key;
- retryable failures produce `ATTENTION`;
- terminal failures produce `INCIDENT_CANDIDATE` without incident authority;
- bounded backlog thresholds produce `ATTENTION`;
- channel output is deterministic;
- unavailable evidence remains fail-closed and authority-free.

`notification_delivery_evidence_repository_tests` additionally verifies restart-safe idempotency, retry recovery, terminal-state persistence, bounded-query fail-closed behavior and durable cross-surface consumption. `notification_dispatch_runtime_tests` verifies bounded provider execution, queue saturation evidence, restart scheduling, cancellation and final projection integration.

The notification regressions remain attached to the sanitizer build graph for ASan, UBSan and TSan coverage. Durable schema and retention details are documented in `NOTIFICATION_DELIVERY_PERSISTENCE.md`.
