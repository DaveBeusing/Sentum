# Sentum Production Deployment & Operational Validation

Copyright (C) 2026 Dave Beusing <david.beusing@gmail.com>
SPDX-License-Identifier: MIT

## Purpose

AP-12 defines the evidence boundary between automated rehearsal and a real production deployment acceptance.

Automated CI may prove that backup/restore mechanics, artifact rollback mechanics and the documented incident-recovery sequence are executable in a controlled rehearsal. CI must never label those results as proof that a production deployment, production restore, exchange reconciliation or incident exercise actually occurred.

## Evidence classes

### CI rehearsal evidence

`tools/ci/operational_validation_rehearsal.py` produces commit-bound, machine-readable evidence for:

- SQLite backup and restore integrity using a sentinel record and `PRAGMA quick_check`;
- candidate-to-previous artifact rollback with SHA-256 identity verification;
- the documented incident-recovery sequence, including entry halt, preservation of execution truth, reconciliation and explicit approval before resume.

The report must contain `environment_class = ci_rehearsal` and must explicitly state that no production target or live exchange was contacted.

### Production acceptance evidence

A real production acceptance record is separate and must be created by the deployment operator after executing the production runbook against the intended target environment.

Required fields:

- Git SHA;
- RC archive name and SHA-256;
- deployed binary SHA-256;
- target environment identifier;
- deployment start and end UTC timestamps;
- operator and approver identifiers;
- pre-deployment backup/recovery-point identifier;
- backup verification result;
- restore or rollback drill/reference result;
- runtime health result;
- market connectivity result;
- persistence queue/pressure result;
- exchange/account reconciliation result where applicable;
- kill-switch state;
- entries-paused state;
- observation-window start/end UTC timestamps;
- incidents or anomalies observed;
- rollback target;
- final disposition: `ACCEPTED`, `ROLLED_BACK`, or `REJECTED`.

No field may be synthesized from CI rehearsal evidence.

## Deployment validation sequence

1. Verify the RC archive SHA-256 and internal manifest/checksums.
2. Verify Git SHA equality across RC package, release-readiness evidence and intended deployment record.
3. Capture a restorable pre-deployment recovery point according to `PRODUCTION_OPERATIONS.md`.
4. Keep entries fail-closed during promotion.
5. Deploy the exact verified binary/configuration approved for the environment.
6. Verify runtime lifecycle/health and market connectivity.
7. Verify persistence queue pressure is acceptable and no saturation is present.
8. Reconcile exchange-confirmed order/position/account state where applicable.
9. Confirm no unexpected kill-switch or safety-state condition exists.
10. Start a bounded observation window before any explicit approval to enable entries.
11. Record incidents/anomalies and rollback if any release-blocking condition appears.
12. Complete the production acceptance record with final disposition.

## Rollback acceptance

Rollback is successful only when:

- the selected prior artifact is positively identified by SHA-256;
- the deployed binary matches that artifact after rollback;
- runtime health returns to the expected state;
- persistence is healthy;
- exchange/account reconciliation succeeds where applicable;
- the handoff record captures the rollback reason and timestamps.

A successful CI artifact rollback rehearsal does not replace this production evidence.

## Incident recovery acceptance

For a material runtime, market-data, persistence or execution anomaly:

1. halt new entries or preserve the existing fail-closed state;
2. preserve execution truth and diagnostic evidence;
3. capture runtime health, queue pressure and relevant logs/metrics;
4. reconcile exchange-confirmed state before making position/order assumptions;
5. select recovery or rollback based on observed truth;
6. restore service in a controlled state;
7. resume entries only after explicit operator approval and required reconciliation.

## Production observation window

The production acceptance record must state a concrete observation window. During that interval the operator must at minimum observe:

- runtime lifecycle/health;
- market connectivity;
- persistence queue depth/pressure and saturation events;
- unexpected drops/backpressure;
- kill-switch and entries-paused state;
- order/trade/position reconciliation where applicable;
- process restarts or crashes;
- operator-visible critical/warning states.

AP-12 does not prescribe a universal duration because the correct window depends on the actual target, market session and release risk. The duration used must be recorded explicitly.

## Fail-closed rules

Production acceptance is not `ACCEPTED` when any required evidence is missing, stale, belongs to another commit/artifact, reconciliation is incomplete, runtime health is abnormal, persistence is saturated, or safety state requires entries to remain paused.

Automated tooling must not clear kill switches, synthesize fills, infer exchange truth, or mark a production deployment accepted from CI rehearsal evidence alone.
