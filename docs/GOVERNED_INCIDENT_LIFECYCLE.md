# Governed Incident Lifecycle

## Purpose

Sentum persists one authoritative operations incident lifecycle behind the existing notification `OPEN_INCIDENT` proposal. Notification delivery evidence may create an approval-required request, but it never approves the request or mutates incident state directly.

The lifecycle remains an operations-control-plane concern. It does not become trading, Risk, Execution or exchange-state authority.

## Durable state

The configured runtime SQLite database stores:

- incident requests;
- approval decisions;
- incident current state;
- reconciliation evidence associated with recovery;
- immutable transition history.

Request submission is idempotent by stable source-correlation identity. Repeated observation of the same notification proposal therefore reuses the existing request instead of opening competing approval paths.

Every persisted lifecycle record keeps `execution_authorized = false`.

## Lifecycle

A notification incident follows this sequence:

1. terminal notification delivery evidence produces `PROPOSAL_READY`;
2. the lifecycle runtime submits or reuses an `OPEN_INCIDENT` request with `APPROVAL_REQUIRED`;
3. an explicit identified operator approves or denies the request;
4. approval opens the incident; denial creates no incident;
5. an open incident may be explicitly acknowledged;
6. recovery may begin only from `ACKNOWLEDGED` and requires a reconciliation evidence identifier;
7. an acknowledged or recovering incident may be explicitly resolved;
8. a resolved incident may be explicitly closed.

The persisted incident states are:

- `OPEN`;
- `ACKNOWLEDGED`;
- `RECOVERY_IN_PROGRESS`;
- `RESOLVED`;
- `CLOSED`.

The request states are:

- `APPROVAL_PENDING`;
- `APPROVED`;
- `DENIED`.

Invalid, stale or mismatched transitions fail closed.

## Approval semantics

`OPEN_INCIDENT` is classified as `APPROVAL_REQUIRED` by the operations governance policy.

Approval is bound to:

- request identity;
- approver identity;
- reason;
- decision timestamp;
- source-correlation evidence.

An approval can advance only a still-pending request. A repeated decision, a mismatched expected correlation or a missing request is rejected.

Denial records an immutable decision and never creates an incident.

## Acknowledgement and resolution

Acknowledgement means only that an identified operator has explicitly acknowledged the open incident. It does not mean:

- recovery has started;
- reconciliation has succeeded;
- a kill switch may be cleared;
- entries may resume;
- Risk or Execution state may change.

Resolution is also independent from trading resumption. Closing or resolving an incident never clears safety interlocks or authorizes trading.

## Recovery

Recovery may begin only after acknowledgement and only with a non-empty reconciliation evidence identifier.

The lifecycle preserves that reconciliation identifier through resolution and closure so restart does not discard the evidence that justified the recovery transition.

Recovery state is operations evidence only. It cannot synthesize fills or replace exchange-confirmed Testnet execution truth.

## Restart behavior

Current request, incident and recovery state is reconstructed from SQLite. No in-memory-only transition is required for recovery after restart.

The lifecycle runtime polls durable notification evidence and performs only idempotent request submission. On restart it can rediscover the same terminal notification failure without duplicating the request or reopening an already-decided incident.

Missing or unreadable notification evidence creates no incident request. Existing durable incident state remains available independently.

## Operator commands

Mutating operations are available only through the controlled Sentum CLI:

```text
sentum incident status
sentum incident approve <request-id> <actor> <reason>
sentum incident deny <request-id> <actor> <reason>
sentum incident acknowledge <incident-id> <actor> <reason>
sentum incident recover <incident-id> <actor> <reconciliation-evidence-id> <reason>
sentum incident resolve <incident-id> <actor> <reason>
sentum incident close <incident-id> <actor> <reason>
```

Reasons containing spaces must be passed as one shell argument.

The browser does not expose any incident write route.

## Cross-surface projection

Terminal and `GET /api/operations` consume the persisted lifecycle projection.

The web path opens the lifecycle database read-only. If authoritative lifecycle persistence cannot be read, incident and recovery state are projected as unavailable and governance evidence is marked stale rather than substituting a healthy state.

Approval and audit projections are bounded for operator presentation. The SQLite transition history remains the durable source.

## Storage failure

Write transitions are transactional. A failed write rolls back the transition and does not advance the authoritative state.

Read-only repositories reject mutation attempts.

Missing, stale, mismatched or incomplete evidence must not grant authority or synthesize a transition.

## Authority boundaries

The incident lifecycle never:

- clears a kill switch;
- resumes entries;
- approves recovery implicitly;
- mutates Strategy, Risk or Execution;
- creates fills;
- changes exchange-confirmed execution truth;
- enables production trading.

Incident completion is deliberately independent from trading recovery and acceptance.
