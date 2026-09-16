# Cross-Surface Operations Contract

## Purpose

AP-18 establishes one presentation contract for production operations state across the terminal and the read-only web dashboard.

The contract is derived from the same dashboard snapshot already used by the runtime surfaces. It does not create a second source of operational truth and does not add browser-side execution authority.

## Canonical view

`CrossSurfaceOperationsView.hpp` projects the shared runtime snapshot into schema version `1` with these sections:

- `runtime` — operator severity, label, message and recommended workspace;
- `governance` — governance state, evidence state, maintenance, incident, recovery and pending approvals;
- `workflows` — maintenance, incident and recovery presentation records;
- `approval_queue` — bounded approval evidence;
- `audit_timeline` — bounded read-only audit evidence.

The top-level authority is always `READ_ONLY_PRESENTATION`.

## Consistency rules

Terminal and web surfaces must preserve identical semantics for:

- runtime severity precedence;
- governance state;
- evidence state (`AVAILABLE`, `MISSING`, `STALE`);
- maintenance, incident and recovery state;
- approval classification;
- stale-evidence fail-closed behavior;
- missing-governance visibility;
- approval and audit totals.

The presentation contract reuses the AP-17 operator policies rather than reimplementing them in browser JavaScript.

## Web API

The dashboard server exposes:

`GET /api/operations`

The endpoint is read-only and is derived from the same merged runtime state used by `/api/status`.

`/api/health` advertises `operations_dashboard: true` when this contract is available.

No POST, PUT, PATCH or DELETE operational endpoint is introduced.

## Authority boundary

The cross-surface view cannot:

- enable live trading;
- clear a kill switch;
- resume entries;
- acknowledge or resolve incidents authoritatively;
- approve maintenance or recovery actions;
- mutate approval or audit evidence;
- synthesize fills;
- mutate Risk or Execution truth;
- override exchange-confirmed execution truth.

Every approval item emitted by the view keeps `execution_authorized = false`.

## Failure behavior

Missing operations-control-plane state remains `UNAVAILABLE / MISSING`.

Stale evidence remains `STALE`; approval rows are projected as `FORBIDDEN / BLOCKED - STALE EVIDENCE`, matching the terminal behavior from AP-17.

Unknown or missing action classifications remain fail-closed through the existing operator-action presentation policy.

## Regression coverage

`cross_surface_operations_view_tests` verifies:

- healthy runtime/governance projection;
- read-only authority boundary;
- approval classification parity;
- stale evidence fail-closed behavior;
- missing evidence visibility.

The test target is attached to the existing sanitizer dependency graph so ASan, UBSan and TSan build it before CTest runs.

## Next integration slice

The next AP-18 slice will make the browser Runtime view consume `/api/operations` directly and render the same severity, governance, evidence, workflow and approval semantics used by the terminal.
