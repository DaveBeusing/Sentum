# Durable Notification Delivery Evidence

## Purpose

Notification delivery evidence is persisted in the configured runtime SQLite database so delivery state, idempotency and operational observability survive process restarts.

The persistence boundary is append-only. Historical delivery transitions are immutable during normal runtime operation, and current state is derived from the latest persistent sequence for each generation-safe delivery dedup key.

## Ownership

`NotificationDeliveryEvidenceRepository` is the single persistence boundary for notification delivery evidence.

The governed notification dispatch runtime, when enabled, is the only component permitted to open this repository in read-write mode and append delivery transitions. Operations, dashboard and incident-workflow consumers open the same database read-only.

Read-only consumers never create the database, create the schema or append delivery evidence. Missing or unreadable persistence therefore remains distinguishable from a valid empty evidence store.

## Storage location

The repository uses the configured runtime database path exposed as `db_path`. No second notification-specific persistence technology or independent database configuration is introduced.

The repository follows the existing SQLite conventions:

- SQLite is opened with a 5 second busy timeout;
- the writer uses WAL mode;
- the writer uses `synchronous=NORMAL`;
- schema creation occurs when the read-write repository is constructed;
- observability uses a read-only SQLite connection.

## Schema

The append-only table is `notification_delivery_evidence`.

Each row contains:

- `sequence INTEGER PRIMARY KEY AUTOINCREMENT`;
- `dedup_key`;
- `alert_id`;
- `generation`;
- `channel`;
- `audience`;
- `state`;
- `attempt`;
- `max_attempts`;
- `retry_backoff_seconds`;
- `terminal`;
- `provider_reference`;
- `failure_code`;
- `failure_reason`;
- `observed_at_utc`;
- `delivery_authorized`;
- `execution_authorized`.

`execution_authorized` is constrained to `0` in the database. The repository also rejects an in-memory record that attempts to set execution authorization.

Valid delivery states are `PENDING`, `DISPATCHED`, `DELIVERED` and `FAILED`.

## Ordering

Persistent `sequence` is the authoritative ordering for delivery evidence.

Wall-clock timestamps are evidence only. They are not used to determine which transition is current because timestamps can be absent, duplicated or affected by clock behavior.

Recent-history queries return records in ascending persistent sequence within the selected bounded window. Latest-state queries select the maximum persistent sequence for each dedup key.

## Idempotency

The durable transition identity is indexed by:

`dedup_key, state, attempt, provider_reference, failure_code`

Replaying the same transition after a restart is ignored by `INSERT OR IGNORE` and does not create a second logical transition.

A later transition for the same dedup key remains appendable. A later alert generation remains independently deliverable because generation participates in the generation-safe dedup key produced by the routing policy.

## Read behavior

All repository reads are bounded. The hard repository query ceiling is 4096 records.

Default limits are:

- recent diagnostic history: 128 records;
- latest state per dedup key: 1024 records;
- restart recovery: 1024 current dedup keys.

A result reports its total logical row count and whether the requested result was truncated.

Current-state observability and restart recovery never accept a truncated latest-state set as complete truth. Truncation causes a fail-closed result instead of silently deriving health or retry behavior from incomplete evidence.

The dashboard reads only the bounded latest state per dedup key. It does not reload the full historical table on each refresh.

## Restart recovery

Restart recovery loads the latest persistent transition for every dedup key and restores only non-terminal work:

- `PENDING`;
- `DISPATCHED`;
- non-terminal `FAILED`.

Retry recovery preserves:

- current attempt;
- maximum attempts;
- deterministic retry backoff;
- provider and failure evidence;
- notification delivery authorization.

`DELIVERED` and terminal `FAILED` records remain terminal and are not restored as active work.

All restored delivery attempts explicitly retain `execution_authorized = false`.

## Operational observability

The operations projection consumes the durable latest-state set and applies the existing notification-health reduction semantics.

The cross-surface operations view replaces any volatile notification-delivery array in the runtime snapshot with durable evidence before deriving notification health and the governed incident candidate.

If the durable database is missing, cannot be opened, does not contain the required table, is corrupt, or cannot provide a complete bounded latest-state set, notification operations are reported as `UNAVAILABLE`. The incident candidate therefore remains fail-closed and no incident-opening action is authorized.

Existing JSON contracts remain unchanged. The retry-context fields are additive evidence fields.

## Retention and archival

Normal runtime state transitions never update or delete historical delivery rows.

The active SQLite table has an operational soft limit of 100,000 notification-delivery rows. Reaching that threshold is an archival condition, not permission for automatic deletion in the runtime hot path.

Archival must obey all of the following rules:

1. archive rows to durable immutable storage before removing them from the hot database;
2. preserve the latest row for every dedup key in the hot database;
3. preserve all rows required to explain active or retriable delivery work;
4. preserve terminal-failure evidence associated with an unresolved incident or recovery workflow;
5. preserve enough history to demonstrate the transition chain for any unresolved operational investigation;
6. perform archival as an explicit maintenance operation, never as part of a delivery state transition;
7. validate the archive before any hot-store deletion;
8. leave notification health fail-closed if archival cannot prove that required current evidence remains available.

No automatic destructive retention job is introduced by this persistence boundary.

## Authority isolation

Notification persistence carries delivery evidence only.

Neither the repository nor any read-only projection may:

- create, acknowledge, resolve or close incidents;
- acknowledge or resolve operator alerts;
- clear kill switches;
- resume entries;
- approve governed actions;
- mutate Risk or Execution state;
- synthesize fills;
- override exchange-confirmed execution truth.

Persistent delivery evidence can never authorize trading or execution.

## Validation

Regression coverage verifies:

- append and read-back;
- exact field preservation;
- monotonic persistent ordering;
- duplicate suppression across repository reopen;
- multiple transitions for one dedup key;
- independent alert generations;
- terminal-state persistence across restart;
- retry-context recovery;
- bounded query behavior and fail-closed truncation;
- missing and corrupt persistence failure behavior;
- read-only consumer enforcement;
- `execution_authorized = false` after persistence and restoration;
- unchanged notification-health semantics;
- unchanged governed incident-candidate and approval/audit/recovery correlation semantics.
