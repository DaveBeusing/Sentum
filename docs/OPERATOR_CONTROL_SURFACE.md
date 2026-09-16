# Operator Control Surface

## Purpose

AP-17 defines the operator-facing control-surface contract that combines runtime safety, workspace context and operational-governance evidence without creating a second trading authority.

The surface is presentation-only. It must never infer permission to trade, clear safety interlocks, synthesize execution truth or mutate Risk/Execution state.

## Inputs

The presentation contract consumes existing authoritative or already-derived state:

- runtime health;
- kill-switch state;
- market-data connectivity;
- entry pause state;
- persistence queue pressure;
- active terminal workspace;
- operations-control-plane governance state;
- maintenance state;
- incident state;
- recovery state;
- pending approval count;
- last governed audit action and actor;
- optional governed operator action and upstream action classification;
- optional structured maintenance, incident and recovery workflow records;
- optional structured approval queue and audit timeline evidence.

The control surface does not recompute governance classes. Action classification must be supplied by the upstream control-plane contract.

## Operator hierarchy

The always-visible operator surface presents information in this order:

1. safety/runtime status banner;
2. active workspace and navigation;
3. governance state;
4. maintenance / incident / recovery state;
5. pending approvals;
6. last governed audit action;
7. governed operator-action state;
8. structured maintenance, incident and recovery workflow detail;
9. bounded approval queue and audit timeline evidence;
10. keyboard/focus safety guidance;
11. workspace-specific operational detail.

Critical runtime and safety status always outranks informational governance detail.

## Action presentation

The UI consumes one of three upstream governance classes:

- `AUTOMATED` -> delegated to existing control-plane automation and never locally authorized by the terminal;
- `APPROVAL_REQUIRED` -> explicit operator confirmation creates an approval-request state only;
- `FORBIDDEN` -> blocked and not confirmable.

Unknown action classes fail closed and are presented as blocked.

`OperatorActionFlow.hpp` models only operator UX state:

- `ConfirmationRequired`;
- `ApprovalRequested`;
- `Delegated`;
- `Blocked`;
- `Cancelled`.

The flow deliberately has no executable or authorized state. `execution_authorized` remains false for every transition. Confirming an `APPROVAL_REQUIRED` action therefore means "request approval from the upstream control plane", not "execute the action".

The regression suite covers approval-required, forbidden, unknown, automated and cancelled flows and verifies that none authorizes execution locally.

## Maintenance, incident and recovery workflows

`OperatorWorkflowView.hpp` provides one common presentation contract for governed operational workflows. Each workflow may expose:

- state;
- requested action;
- upstream governance classification;
- request identifier;
- reason;
- actor / initiator;
- approval-required or blocked presentation state.

The supported workflow views are:

- `maintenance_workflow` for entering or leaving controlled maintenance states;
- `incident_workflow` for acknowledgement and governed incident-response steps;
- `recovery_workflow` for recovery-candidate and reconciliation-related approval paths.

These records are views only. They do not execute transitions, acknowledge incidents, promote recovery candidates or resume entries.

Any active workflow action without an upstream classification fails closed as `FORBIDDEN / BLOCKED`. `execution_authorized` remains false for every maintenance, incident and recovery view.

The regression suite verifies:

- maintenance transitions remain approval-gated;
- incident acknowledgement remains governed;
- recovery promotion requires explicit approval;
- missing classifications fail closed;
- absent workflows remain visible as `IDLE` rather than being inferred as approved.

## Approval queue and audit timeline

`OperatorAuditQueueView.hpp` projects upstream governance evidence into a bounded, read-only operator view.

Approval queue rows expose request ID, action, upstream governance classification, actor, reason and presentation status. Missing or unknown approval classifications fail closed as `FORBIDDEN / BLOCKED`. Approval rows never authorize execution locally.

Audit timeline rows expose UTC timestamp, request ID, action, actor, reason and recorded outcome. The terminal does not create, modify, reorder, approve or delete upstream audit evidence. It is a viewer only.

The projection is deliberately bounded for terminal rendering: the production surface displays at most four approval rows and five audit rows per frame and visibly reports when additional evidence was omitted from the terminal view. Full immutable history remains an operations-control-plane responsibility.

## Operator navigation, focus and keyboard safety

`OperatorNavigationPolicy.hpp` defines a deterministic UI-only navigation state machine for three focus regions:

- approval queue;
- audit timeline;
- workflow detail.

Navigation bindings are deliberately separated from action authority:

- `j` / `k` move selection only;
- `Tab` or `]` moves focus forward;
- `[` moves focus backward;
- `Enter` opens the selected view or, for an `APPROVAL_REQUIRED` row, opens confirmation UX only;
- `Esc` cancels the local interaction state.

A second `Enter` while confirmation is already open does not execute or authorize anything. Forbidden approval rows never open confirmation. Audit and workflow detail remain read-only. Every navigation transition forces `execution_authorized = false`.

The terminal surface displays this keyboard contract explicitly so an operator can distinguish navigation keys from existing runtime-control hotkeys. The navigation policy itself is side-effect free and does not call RuntimeControl, Risk, Execution or persistence APIs.

## Missing governance evidence

Missing operations-control-plane evidence remains visible as `UNAVAILABLE`.

The presentation must not silently substitute `CONTROLLED`, zero pending approvals, or any other healthy default when governance evidence is absent.

## Audit presentation

When audit evidence exists, the operator surface presents the most recent governed action and actor. Structured audit timeline rows add request ID, timestamp, outcome and reason where provided. A full immutable audit chain remains an operations-control-plane concern; the terminal is a viewer, not the audit authority.

Workflow request IDs, reasons and actors are included as operator context when provided. They are presentation evidence only and do not replace the immutable upstream audit record.

## Terminal integration

`TerminalOperatorSurfaceRenderer.hpp` converts the tested operator contracts into deterministic terminal frame lines for the always-visible operator area. It contains no repository access, filesystem access, sleep, locking or control mutation.

The generated surface contains:

1. the operator safety/runtime banner;
2. canonical workspace navigation and active-workspace marker;
3. workspace purpose/context;
4. governance, maintenance, incident and recovery state;
5. pending-approval summary and most recent governed audit action;
6. optional governed action state from `operations_control_plane.operator_action`;
7. structured maintenance, incident and recovery workflow summaries;
8. bounded approval queue and audit timeline rows;
9. keyboard/focus safety guidance.

The production `TerminalUi` hook combines these lines with the existing terminal frame before the AP-06 diff is calculated. It reuses the cached dashboard snapshot and active tab and introduces no second snapshot read, polling path or output stream.

The terminal-render regression suite requires an identical second operator frame, including workflow, approval and audit rows, to produce zero payload bytes and zero changed rows.

## Sanitizer regression integration

The Core CI sanitizer jobs build an explicit legacy regression-target list. New AP-17 regression executables are attached as CMake dependencies of the already-built `sentum_operational_safety_policy_tests` target. This guarantees that ASan, UBSan and TSan build the operator action, workflow, audit/approval and navigation regressions before CTest executes them, avoiding registered-but-unbuilt test executables.

## Performance and rendering invariant

AP-17 preserves AP-06 terminal rendering behavior:

- presentation helpers remain deterministic and side-effect free;
- unchanged source state produces unchanged frame content;
- unchanged frame content results in zero terminal output bytes through the existing diff renderer;
- approval/audit projections are bounded;
- navigation state transitions are constant-time and allocate no external resources;
- no new repository polling, filesystem polling or blocking I/O is introduced into the render hot path.

## Non-goals

This package does not:

- enable live trading;
- clear kill switches;
- resume entries directly;
- approve maintenance or recovery transitions locally;
- acknowledge or resolve incidents locally;
- promote recovery candidates locally;
- mutate approval or audit evidence;
- create or accept reconciliation truth;
- synthesize fills;
- mutate Risk or Execution state;
- replace exchange-confirmed execution truth.
