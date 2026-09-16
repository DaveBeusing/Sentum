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
- last governed audit action and actor.

The control surface does not recompute governance classes. Action classification must be supplied by the upstream control-plane contract.

## Operator hierarchy

The always-visible operator surface should present information in this order:

1. safety/runtime status banner;
2. active workspace and navigation;
3. governance state;
4. maintenance / incident / recovery state;
5. pending approvals;
6. last governed audit action;
7. workspace-specific operational detail.

Critical runtime and safety status always outranks informational governance detail.

## Action presentation

The UI consumes one of three upstream governance classes:

- `AUTOMATED` -> available inside existing guardrails, no operator confirmation implied by the presentation contract;
- `APPROVAL_REQUIRED` -> explicit operator confirmation is required before execution;
- `FORBIDDEN` -> blocked and must not be executable.

Unknown action classes fail closed and are presented as `FORBIDDEN / BLOCKED`.

The UI must never convert a forbidden or unknown action into an executable action based on local state.

## Missing governance evidence

Missing operations-control-plane evidence must remain visible as `UNAVAILABLE`.

The presentation must not silently substitute `CONTROLLED`, zero pending approvals, or any other healthy default when governance evidence is absent.

## Audit presentation

When audit evidence exists, the operator surface presents the most recent governed action and actor. A full immutable audit chain remains an operations-control-plane concern; the terminal is a viewer, not the audit authority.

## Performance and rendering invariant

AP-17 must preserve AP-06 terminal rendering behavior:

- presentation helpers remain deterministic and side-effect free;
- unchanged source state must produce unchanged frame content;
- unchanged frame content must result in zero terminal output bytes through the existing diff renderer;
- no new repository polling, filesystem polling or blocking I/O is introduced into the render hot path.

## Current integration boundary

This package establishes the tested presentation contract in `TerminalWorkspacePolicy.hpp` and extends the existing `terminal_workspace_policy` regression target.

The next AP-17 slice integrates the contract into the production `TerminalUi` always-visible header/navigation and adds explicit approval/maintenance/incident/recovery panels while preserving the zero-write invariant.

## Non-goals

This package does not:

- enable live trading;
- clear kill switches;
- resume entries;
- approve maintenance or recovery transitions;
- create or accept reconciliation truth;
- synthesize fills;
- mutate Risk or Execution state;
- replace exchange-confirmed execution truth.
