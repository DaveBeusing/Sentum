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
- optional governed operator action and upstream action classification.

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
8. workspace-specific operational detail.

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

## Missing governance evidence

Missing operations-control-plane evidence remains visible as `UNAVAILABLE`.

The presentation must not silently substitute `CONTROLLED`, zero pending approvals, or any other healthy default when governance evidence is absent.

## Audit presentation

When audit evidence exists, the operator surface presents the most recent governed action and actor. A full immutable audit chain remains an operations-control-plane concern; the terminal is a viewer, not the audit authority.

## Terminal integration

`TerminalOperatorSurfaceRenderer.hpp` converts the tested `OperatorControlSurface` contract into deterministic terminal frame lines for the always-visible operator area. It contains no repository access, filesystem access, sleep, locking or control mutation.

The generated surface contains:

1. the operator safety/runtime banner;
2. canonical workspace navigation and active-workspace marker;
3. workspace purpose/context;
4. governance, maintenance, incident and recovery state;
5. pending-approval summary and most recent governed audit action;
6. optional governed action state from `operations_control_plane.operator_action`.

The production `TerminalUi` hook combines these lines with the existing terminal frame before the AP-06 diff is calculated. It reuses the cached dashboard snapshot and active tab and introduces no second snapshot read, polling path or output stream.

The terminal-render regression suite requires an identical second operator frame to produce zero payload bytes and zero changed rows.

## Performance and rendering invariant

AP-17 preserves AP-06 terminal rendering behavior:

- presentation helpers remain deterministic and side-effect free;
- unchanged source state produces unchanged frame content;
- unchanged frame content results in zero terminal output bytes through the existing diff renderer;
- no new repository polling, filesystem polling or blocking I/O is introduced into the render hot path.

## Non-goals

This package does not:

- enable live trading;
- clear kill switches;
- resume entries directly;
- approve maintenance or recovery transitions locally;
- create or accept reconciliation truth;
- synthesize fills;
- mutate Risk or Execution state;
- replace exchange-confirmed execution truth.
