# Continuous Production Readiness

Copyright (C) 2026 Dave Beusing <david.beusing@gmail.com>

## Purpose

AP-13 defines the monitoring and evidence baseline for continuous production readiness after release-candidate and operational-validation gates have succeeded.

This document does not authorize live trading. It defines when the runtime may be considered operationally ready, when operator attention is required, and when activity must remain blocked.

## Authoritative runtime signals

Continuous readiness reuses existing runtime presentation truth. The minimum monitored fields are:

- `health`;
- `kill_switch_active`;
- `market_data_connected`;
- `performance.queue_pressure`.

No second lifecycle or parallel trading-state authority is introduced.

## Readiness states

### READY

All of the following are required:

- runtime health is `healthy`;
- kill switch is inactive;
- market data is connected;
- persistence queue pressure is neither `critical` nor `saturated`.

READY is an operational observation state. It is not permission to bypass Risk, Execution, reconciliation or explicit operator controls.

### ATTENTION

ATTENTION applies when runtime health remains healthy but an operational dependency is degraded, including:

- market-data disconnection;
- persistence queue pressure `critical` or `saturated`.

An ATTENTION sample invalidates a clean production observation window. New operational promotion decisions must remain fail-closed until the condition is resolved and a fresh clean window is observed.

### BLOCKED

BLOCKED applies when:

- kill switch is active;
- runtime health is not `healthy`;
- runtime is stopping or stopped.

BLOCKED must never be interpreted as production ready. Resume remains an explicit operator decision after root-cause resolution and reconciliation where applicable.

## Observation windows

A production observation window records timestamped samples of the authoritative runtime signals.

A clean window requires every required sample to classify as READY. Any ATTENTION or BLOCKED sample invalidates the clean window and starts a new observation period after recovery.

The observation record must include:

- Git SHA and deployed artifact identity;
- environment identifier;
- UTC start/end timestamps;
- sample cadence and sample count;
- readiness state for every sample;
- market-data connectivity changes;
- persistence pressure changes;
- kill-switch transitions;
- incident references, if any;
- operator/approver identifiers;
- final disposition.

No fixed real-production duration is asserted by CI rehearsal evidence. The target-environment acceptance process owns the actual observation duration.

## Incident escalation

The minimum escalation rules are:

1. BLOCKED state: halt promotion/resume decisions immediately.
2. Kill switch active: preserve the halt until an operator explicitly clears it after investigation.
3. Runtime health abnormal: inspect SYSTEM/runtime evidence before recovery.
4. Market disconnected: treat current market state as unreliable for production acceptance.
5. Critical/saturated persistence pressure: inspect queue depth, high-water and writer diagnostics.
6. Preserve logs, runtime evidence and exchange-confirmed state before remediation.
7. Reconcile external execution truth before any resume decision where execution may have occurred.

## Continuous-readiness evidence

CI produces `continuous_readiness_rehearsal.json` and `.md` using `environment_class = ci_rehearsal`.

The rehearsal must prove at minimum that:

- healthy/connected/normal-pressure samples classify READY;
- market disconnection does not classify READY;
- critical or saturated persistence pressure does not classify READY;
- active kill switch classifies BLOCKED;
- abnormal runtime health classifies BLOCKED;
- a degraded sample invalidates an otherwise healthy observation window.

## Evidence boundary

A CI rehearsal PASS proves only that the repository monitoring policy behaves as designed for the exercised scenarios. It is not evidence that a real production observation window, incident response, exchange reconciliation or recovery action occurred.

Real production readiness requires target-environment evidence and explicit operational acceptance.

## Fail-closed rule

Missing, malformed, stale or commit-mismatched production evidence must be treated as not ready. Unknown states must not be silently promoted to READY.
