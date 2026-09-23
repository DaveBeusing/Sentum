# Operator Control Surface & Production Operations UX Acceptance

## Purpose

This document defines the final acceptance contract for the operator control surface. It consolidates the operator-facing safety, navigation, rendering and authority boundaries implemented across the feature.

The operator control surface is accepted only when the final pull-request head passes the complete Core CI matrix. Earlier green runs are supporting evidence only and do not replace final-head evidence.

## Functional acceptance

The production terminal must provide all of the following:

- an always-visible operator safety/runtime banner;
- canonical workspace context and navigation;
- governance, maintenance, incident and recovery status;
- bounded approval queue and audit timeline views;
- operator-action, maintenance, incident and recovery workflow presentation;
- modal operator navigation with visible focus and selection;
- explicit confirmation, blocked, cancelled, empty, missing and stale evidence states;
- deterministic rendering through the existing terminal diff pipeline.

## Input safety acceptance

The production `TerminalUi` input boundary must satisfy all of the following:

- `O` enters operator-navigation mode;
- while operator navigation is active, workspace and runtime-control hotkeys are suppressed;
- `j` and `k` move selection only;
- `Tab`, `]` and `[` move focus only;
- `Enter` can open detail or approval-confirmation UX only;
- repeated `Enter` cannot authorize or execute an action;
- `Esc` exits or cancels local operator interaction without execution;
- normal terminal input ownership is restored only after modal operator navigation closes.

## Evidence and failure acceptance

Operator evidence must fail closed:

- missing operations-control-plane evidence is visible as `MISSING`;
- stale evidence is visible as `STALE`;
- stale approval rows are rendered `FORBIDDEN / BLOCKED - STALE EVIDENCE`;
- approval and audit selections are clamped when bounded queues shrink;
- empty queues are visible as explicit empty states;
- an open confirmation is invalidated if its request disappears, action changes or classification ceases to be `APPROVAL_REQUIRED`;
- no missing, stale, changed or empty evidence path may set `execution_authorized` to true.

## Authority boundary acceptance

The terminal remains presentation-only for governed operations.

The operator control surface must not introduce any code path that directly:

- enables live trading;
- clears a kill switch;
- resumes entries through an approval confirmation;
- acknowledges or resolves an incident authoritatively;
- promotes a recovery candidate authoritatively;
- mutates approval or audit evidence;
- synthesizes fills;
- mutates Risk or Execution truth;
- overrides exchange-confirmed execution truth.

An `APPROVAL_REQUIRED` action may only enter confirmation/approval-request UX. `AUTOMATED` remains delegated to the control plane. `FORBIDDEN` and unknown classifications remain blocked.

## Rendering and performance acceptance

The operator control surface must preserve the terminal frame-pacing rendering contract:

- unchanged source state produces unchanged frame content;
- an unchanged frame produces zero terminal output bytes;
- approval/audit projection remains bounded;
- operator navigation and reconciliation remain in-memory and bounded;
- no new filesystem polling, repository polling, blocking I/O, sleep or second dashboard snapshot read is introduced into the render hot path;
- all operator surface content participates in the same terminal diff and output path.

## Regression acceptance

The final-head Core CI run must execute and pass the existing regression set including:

- terminal workspace policy tests;
- terminal UI policy tests;
- terminal render pipeline tests;
- operational safety policy tests;
- operator action flow tests;
- operator workflow view tests;
- operator audit queue view tests;
- operator navigation policy tests;
- ASan;
- UBSan;
- TSan.

Sanitizer test executables must be built before CTest executes them. Registered-but-unbuilt tests are an acceptance failure.

## Evidence rule

Acceptance status is one of:

- `IMPLEMENTED / CI PENDING` — scope is complete but final-head CI has not succeeded;
- `ACCEPTED` — final-head Core CI and required sanitizer jobs completed successfully;
- `BLOCKED` — final-head CI or required regressions failed.

No documentation text, previous successful run or mergeable GitHub state may substitute for final-head CI evidence.
