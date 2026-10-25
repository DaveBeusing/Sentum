# Sentum Production Operations Baseline

## Purpose

This document defines the minimum operational baseline for promoting a verified Sentum release candidate into a production-like environment. It is downstream of the Release Readiness and RC Handoff contracts and does not authorize live trading by itself. Repository automation concludes with consolidated readiness evidence; a successful `REPOSITORY_READY` result is still distinct from target-environment `TARGET_ACCEPTED` evidence.

## Production promotion prerequisites

A production promotion must not begin unless all of the following are true:

- the exact RC archive and SHA-256 checksum are available;
- the RC manifest and release-readiness evidence refer to the exact Git commit being promoted;
- the archive checksum has been verified before extraction;
- environment-specific configuration and credentials are injected separately from the RC bundle;
- the target environment has a current backup or recovery point for persistent state;
- an operator is identified for promotion, verification and rollback ownership;
- trading remains fail-closed until runtime health, market connectivity and reconciliation are verified.

## Automated RC evidence preflight

The production-operations workflow qualifies **successful Core CI push runs on
`master` in this repository**. Pull-request runs, other branches, other events,
failed runs and foreign source repositories do not qualify and are explicitly
skipped before checkout or artifact download. Core CI still builds, tests,
validates readiness and packages an RC for every pull request.

For an integrated master push, the producer's evaluated `github.sha` equals the
source run's `head_sha`. Checkout, the exact artifact name
`sentum-rc-<full-git-sha>`, every downstream expected SHA and the final evidence
artifact use that same identity. Downloads remain bound to the completed Core CI
run ID, never to the latest artifact with a similar name. A pull request's
synthetic merge SHA is intentionally not replaced with its branch-head SHA.

Representative workflow payload tests cover the historical PR merge/head
mismatch, a successful master push, other branches/events/repositories and
unsuccessful runs. They evaluate the workflow's actual condition and resolve its
checkout/download expressions against those payloads. Final acceptance also
requires the integrated master-push chain to complete with the produced artifact.

After download into `evidence/rc`, the required evidence locations are:

- `evidence/rc/log/rc_package.json`;
- `evidence/rc/log/rc_package.md`;
- `evidence/rc/artifacts/sentum-rc-<first-12-git-sha>.tar.gz`;
- `evidence/rc/artifacts/sentum-rc-<first-12-git-sha>.tar.gz.sha256`.

The handoff validator checks this structure, the RC report status and schema, Git
SHA and source-run equality, archive path, actual archive digest and checksum
evidence before the production-operations gate runs. It also checks the original
readiness report embedded in the archive and the preserved leaf evidence
identities and timestamps. Repackaging cannot extend their validity.

Direct invocation of the production-operations gate independently requires an RC
JSON object with the supported schema, status, commit, source run and complete
release-evidence provenance. Null, scalar, array, malformed or stale input produces
a structured FAIL report and summary; a preceding workflow step is not required
to enforce this boundary. Any mismatch remains fail-closed.

## Deployment procedure

1. Record the RC Git SHA, archive checksum, target environment, operator and deployment start time.
2. Verify the external archive SHA-256 before extraction.
3. Verify the internal `SHA256SUMS` and `MANIFEST.json` after extraction.
4. Confirm that no `config/config.json`, credentials, runtime database or log files are included in the RC artifact.
5. Install the binary and static documentation into the target release directory without overwriting the previous known-good release.
6. Inject environment-specific configuration from the controlled environment store.
7. Start Sentum in the intended non-live or production operating mode while entries remain paused/fail-closed.
8. Verify dashboard/runtime health, market-data connectivity, persistence state and absence of unexpected queue pressure.
9. Perform account/state reconciliation before allowing any live execution mode.
10. Record the final promotion result and the exact binary checksum in the handoff record.

## Rollback procedure

Rollback is required if runtime health is abnormal, reconciliation cannot be completed, market-data state is unreliable, persistence is degraded beyond the accepted operating envelope, startup does not complete, or the deployed binary/evidence cannot be proven to match the approved RC.

1. Keep or activate entries-paused / fail-closed behavior.
2. Stop Sentum using the normal ordered shutdown path when possible.
3. Confirm shutdown reached the terminal stopped state and persistence queues were drained.
4. Restore the previous known-good release directory without modifying its binary.
5. Restore environment configuration from the controlled source rather than from the RC archive.
6. Restore persistent state from the pre-deployment recovery point only when state validation shows it is required.
7. Start the previous known-good release with entries paused.
8. Re-run health, connectivity and reconciliation checks.
9. Record rollback reason, timestamps, affected commit(s), state-recovery actions and final status.

## Backup and recovery baseline

Persistent state must have a documented recovery point before promotion. A recovery record must contain:

- backup identifier and creation time;
- source environment and database/storage location;
- Git SHA and release identifier active when the backup was created;
- integrity verification result;
- restore test status or, when a restore test is not feasible, an explicit `UNVERIFIED` status;
- retention/expiry information;
- operator responsible for recovery validation.

`UNVERIFIED` is never equivalent to `PASS`.

## Incident baseline

### Severity

- **SEV-1:** execution truth, account state, reconciliation or risk controls cannot be trusted; keep trading fail-closed and escalate immediately.
- **SEV-2:** material runtime degradation, repeated persistence pressure, market-data instability or failed recovery with no confirmed execution-truth impact.
- **SEV-3:** operational degradation with bounded impact and a working recovery path.

### First-response order

1. protect execution truth and keep entries fail-closed;
2. capture current operational state, Git SHA, RC checksum and relevant logs/metrics;
3. determine whether exchange/account reconciliation is required;
4. decide recover-in-place versus rollback;
5. preserve evidence before destructive recovery actions;
6. document timeline, operator actions and final state.

An incident must never be resolved by synthesizing fills, clearing a kill switch without validating its cause, or mutating Risk/Execution state solely to make the UI appear healthy.

## Post-deployment verification

The deployment handoff is complete only when all applicable checks are recorded:

- runtime health is healthy;
- market-data connectivity is confirmed;
- persistence is not saturated/critical;
- shutdown/restart path remains available;
- account/state reconciliation is complete where required;
- exact deployed binary checksum is recorded;
- rollback target is known and still available;
- operator ownership is transferred explicitly.

## Required production handoff record

Each promotion must capture at minimum:

- Git SHA;
- RC archive name and SHA-256;
- deployed binary SHA-256;
- target environment;
- deployment start/end UTC timestamps;
- operator/approver identifiers;
- pre-deployment backup/recovery-point identifier;
- release-readiness result;
- reconciliation result where applicable;
- rollback target;
- final status: `PASS`, `ROLLED_BACK`, `FAILED`, or `UNVERIFIED`.

A missing required field prevents the handoff from being treated as complete.

## Consolidated readiness boundary

The production-operations workflow emits `consolidated_readiness.json` after the operations, continuous-readiness, reliability, resilience and control-plane rehearsal gates have completed. The repository-generated result may reach `REPOSITORY_READY`; it cannot synthesize target-environment acceptance. See [Readiness Evidence Contract](READINESS_EVIDENCE.md) and [Production Validation](PRODUCTION_VALIDATION.md).
