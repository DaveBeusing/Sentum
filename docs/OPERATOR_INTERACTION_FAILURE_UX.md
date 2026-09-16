# Operator Interaction Failure UX

## Purpose

This AP-17 slice hardens the production terminal against changing, missing or stale operations-control-plane evidence without moving authority into the UI.

The terminal remains a presentation and navigation surface. It never authorizes trading, mutates Risk/Execution truth, approves governed actions, clears kill switches or synthesizes fills.

## Evidence states

The operator evidence projection distinguishes three states:

- `AVAILABLE`: approval and audit evidence can be browsed normally;
- `MISSING`: no operations-control-plane evidence is available, so the operator surface remains read-only and explicitly reports the gap;
- `STALE`: evidence exists but is not current, so approval rows are forced to `FORBIDDEN / BLOCKED - STALE EVIDENCE`.

`operations_control_plane.evidence_status = "STALE"` or `operations_control_plane.evidence_stale = true` marks stale evidence. Missing `operations_control_plane` data is treated as `MISSING`.

## Selection reconciliation

Operator navigation is reconciled against every current evidence projection before a navigation command is applied.

- approval and audit indices are clamped when queues shrink;
- an empty approval queue reports `APPROVAL QUEUE EMPTY`;
- an empty audit timeline reports `AUDIT TIMELINE EMPTY`;
- selection never indexes outside the bounded projection.

## Confirmation invalidation

An open confirmation is valid only while the selected approval request still exists with the same request ID, action and `APPROVAL_REQUIRED` classification.

If the request disappears, the action changes or the classification changes, the confirmation is closed and the UI reports:

`SELECTION CHANGED - CONFIRMATION CANCELLED`

Missing or stale evidence also closes any open confirmation.

No invalidation path can set `execution_authorized` to true.

## Rendering

The always-visible operator surface now renders:

- evidence state;
- explicit missing/stale warnings;
- empty approval/audit states;
- fail-closed stale approval rows;
- visible confirmation context;
- blocked/cancelled operator notices.

The render projection reconciles navigation state before drawing selection markers so asynchronously changing queues cannot produce stale row focus.

## Performance and safety

All reconciliation is bounded by the already bounded terminal approval/audit projection. It introduces no new filesystem access, repository polling, blocking I/O, sleep, secondary snapshot read or secondary terminal output path.

Unchanged failure-state frames continue through the AP-06 diff renderer and must emit zero terminal payload bytes.

## Regression coverage

The AP-17 regression suite covers:

- stale evidence forcing approval rows fail-closed;
- missing evidence remaining explicit and empty;
- approval selection clamping after queue shrink;
- confirmation cancellation when the selected request disappears;
- confirmation cancellation when classification changes;
- stale/missing evidence preventing confirmation;
- visible stale/missing terminal states;
- unchanged failure-state frames producing zero terminal writes.
