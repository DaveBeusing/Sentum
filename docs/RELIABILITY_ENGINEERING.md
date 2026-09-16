# Sentum Reliability Engineering Baseline

Copyright (C) 2026 Dave Beusing <david.beusing@gmail.com>
SPDX-License-Identifier: MIT

## Purpose

AP-14 defines the first explicit reliability-engineering baseline above Sentum's existing release-readiness, production-operations, operational-validation and continuous-readiness evidence.

This document defines engineering policy. It does **not** claim that Sentum has already achieved the configured production availability target in a real environment.

## Reliability policy

The machine-readable policy lives in `config/reliability_policy.json`.

Initial engineering policy:

- evaluation window: 30 days / 43,200 minutes;
- availability target: 99.9%;
- corresponding unavailable-time budget: 43.2 minutes per 30-day window;
- warning threshold: 50% of the error budget consumed;
- critical threshold: 80% consumed;
- blocked threshold: 100% consumed.

These values are policy defaults for reliability engineering and rehearsal. They are not historical production measurements or a customer SLA.

## Upstream authority

Reliability does not introduce a new runtime or trading truth.

The gate consumes the existing continuous-readiness evidence, which itself derives from authoritative presentation/runtime signals including:

- runtime `health`;
- `kill_switch_active`;
- `market_data_connected`;
- persistence `queue_pressure`.

The reliability gate requires upstream continuous-readiness evidence to:

1. be `PASS`;
2. match the same Git SHA;
3. use the expected environment class;
4. contain a passing observation window.

Any mismatch fails closed.

## Error-budget states

Reliability policy maps budget consumption to operational response:

- below 50%: `READY` for the budget dimension;
- from 50% to below 100%: `ATTENTION`;
- at or above 100%: `BLOCKED`.

The 80% threshold is retained as the critical escalation point inside the ATTENTION band and is intended for alert routing and operator priority.

A BLOCKED reliability state cannot authorize entries, clear a kill switch or override Risk/Execution authority.

## Automated checks

`tools/ci/reliability_gate.py` validates:

- policy structure and monotonic thresholds;
- upstream continuous-readiness PASS state;
- Git-SHA binding;
- environment-class binding;
- observation-window PASS state;
- deterministic READY / ATTENTION / BLOCKED budget scenarios.

The resulting evidence is written as JSON and Markdown and is retained with the existing production-operations evidence chain.

## Alerting baseline

Alert routing should use the following minimum semantics:

- READY: no reliability escalation;
- ATTENTION below the critical threshold: operator notification and increased observation;
- ATTENTION at or above the critical threshold: urgent operator escalation and release/deployment caution;
- BLOCKED: fail closed for production-readiness decisions until the budget condition is resolved or an explicitly governed exception is recorded outside the automated gate.

Alerting must not mutate trading state directly. It surfaces evidence and required operator action.

## Recovery automation boundary

Safe automation may:

- collect diagnostics;
- preserve evidence;
- create or verify backups;
- validate known rollback artifacts;
- run health/readiness probes;
- produce incident and reliability records.

Automation must not:

- synthesize fills;
- clear kill switches;
- re-enable entries solely because an alert cleared;
- override exchange-confirmed execution truth;
- mutate Risk/Execution authority without the existing governed path.

## CI rehearsal versus production SLO evidence

CI reliability evidence uses `environment_class = ci_rehearsal` and demonstrates repository policy behavior only.

A real production SLO record must be based on target-environment observation samples and must identify at minimum:

- environment;
- window start/end UTC;
- total observed minutes;
- unavailable minutes;
- availability result;
- error-budget consumed and remaining;
- incident identifiers contributing to unavailability;
- observation data source;
- operator/approver identity;
- Git SHA and deployed artifact SHA-256.

CI rehearsal evidence must never be represented as a real production SLO measurement.

## Fail-closed behavior

Missing policy, missing upstream evidence, mismatched commit, invalid environment class, failed observation window or exhausted error budget prevents a clean reliability result.

Reliability evidence supplements operational and trading safety controls; it never replaces them.
