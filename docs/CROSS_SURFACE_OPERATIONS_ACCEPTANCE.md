# Production Operations Dashboard & Cross-Surface Consistency Acceptance

## Purpose

Cross-surface operations are accepted only when terminal and browser operations surfaces expose the same canonical presentation semantics, the browser remains read-only, semantic drift is detected fail-closed, and the browser transport degrades visibly and recovers without unbounded polling.

Only Core CI evidence for the final pull-request head counts as final acceptance evidence. Earlier successful runs are supporting baselines only.

## Canonical contract acceptance

The operations presentation contract must remain:

- schema version `1`;
- contract identifier `sentum.operations.v1`;
- authority `READ_ONLY_PRESENTATION`.

The contract must expose runtime status, governance/evidence state, maintenance/incident/recovery state, workflow presentation, bounded approval evidence and bounded audit evidence from the shared runtime snapshot.

No browser-side policy may independently reinterpret safety, governance or execution authority.

## Cross-surface parity acceptance

Terminal and browser projections must remain semantically equal for:

- runtime label, message and recommended workspace;
- governance and evidence state;
- maintenance, incident and recovery state;
- pending approvals;
- approval classification/status;
- maintenance, incident and recovery workflow classification/state.

Unsupported schema, contract identifier or authority is drift and must fail closed.

Deliberately modified browser-facing values must be detected by regression tests.

## Browser authority acceptance

The operations dashboard is presentation-only.

The cross-surface operations layer must not introduce operational `POST`, `PUT`, `PATCH` or `DELETE` routes or browser actions that can:

- enable trading;
- clear a kill switch;
- resume entries;
- approve governed actions;
- acknowledge or resolve incidents authoritatively;
- promote recovery authoritatively;
- mutate Risk or Execution state;
- synthesize fills;
- override exchange-confirmed execution truth.

## Transport resilience acceptance

Browser transport state is separate from canonical operations evidence.

Required behavior:

- normal active-view refresh target is 2 seconds;
- refresh uses single-shot scheduling rather than an unconditional interval;
- overlapping `/api/operations` requests are suppressed;
- retry delay grows after failures and is bounded to 30 seconds;
- polling stops when the Operations tab is inactive;
- if no successful snapshot has ever been received, transport failure is `UNAVAILABLE`;
- if a previously successful snapshot exists, a later transport failure marks the browser transport `STALE` while leaving the last rendered canonical snapshot visible;
- stale transport must never be presented as `LIVE`;
- the next valid successful contract resets retry state and visibly returns transport to `LIVE`;
- contract mismatch is `UNAVAILABLE` and fail-closed rather than a recoverable interpretation of unknown semantics.

Transport `STALE` does not rewrite the server-provided operations evidence state. It indicates that the displayed canonical snapshot is no longer freshly confirmed by transport.

## Regression acceptance

Final-head Core CI must execute and pass at least:

- `cross_surface_operations_view`;
- `dashboard_operations_overlay`;
- `cross_surface_semantic_parity`;
- existing terminal/operator regressions;
- ASan;
- UBSan;
- TSan.

The dashboard overlay regression must cover bounded backoff, single-shot scheduling, no overlapping requests, visible transport stale/unavailable state, successful recovery to live state, contract validation and absence of browser write methods.

## Acceptance state

Cross-surface operations acceptance status is one of:

- `IMPLEMENTED / CI PENDING` — scope is complete but final-head CI has not succeeded;
- `ACCEPTED` — final-head Core CI including required regressions and sanitizers completed successfully;
- `BLOCKED` — final-head CI or required acceptance regressions failed.

Mergeable GitHub state, documentation or an earlier green run does not substitute for final-head CI evidence.
