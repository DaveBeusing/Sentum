# Sentum Release Candidate Handoff

## Purpose

This document defines the handoff contract for a Sentum release candidate after the repository release-readiness gate has passed.

The RC package is an immutable operational handoff artifact. It is not permission to enable live trading. Exchange credentials, environment-specific configuration and operational approval remain outside the package. The evidence hierarchy is defined in `READINESS_EVIDENCE.md` and is included in the RC bundle.

## Required RC evidence

An RC is eligible for handoff only when all of the following belong to the exact same Git commit:

- Release build and regression tests passed;
- Performance Gate passed;
- Operational Acceptance passed with all requested cycles completed;
- ASan passed;
- UBSan passed;
- TSan passed;
- Release Readiness passed, including Core CI Runtime Qualification and Independent Research Validation evidence;
- RC package manifest records the same commit SHA;
- archive SHA-256 is published beside the archive.

Missing, stale or commit-mismatched evidence blocks handoff.

## Package contents

The RC bundle contains:

- `bin/sentum` — the stripped Release binary built by CI;
- safe example configuration files plus repository `risk.json`;
- operator, safety, lifecycle and release-readiness documentation;
- `evidence/release_readiness.json`;
- `MANIFEST.json` with file sizes and SHA-256 digests;
- `SHA256SUMS` for files inside the bundle.

## CI artifact handoff layout

The GitHub Actions handoff artifact is named `sentum-rc-<full-git-sha>`. The uploaded artifact preserves one canonical repository-relative layout:

```text
artifacts/sentum-rc-<first-12-git-sha>.tar.gz
artifacts/sentum-rc-<first-12-git-sha>.tar.gz.sha256
log/rc_package.json
log/rc_package.md
```

When the downstream production-operations workflow downloads that artifact into `evidence/rc`, the RC package report is therefore consumed from `evidence/rc/log/rc_package.json`.

Before the production-operations gate executes, `tools/ci/validate_rc_handoff.py`
verifies the exact directory structure, report schema and PASS state, full Git SHA,
source workflow run, expected archive path, archive SHA-256 and checksum-file
contents. It reads the original readiness and manifest from the archive without
extracting paths, validates their complete contracts, and compares the readiness
digest and preserved source records with the RC report. Missing, malformed,
stale or mismatched evidence is a hard failure; alternate fallback locations are
not accepted.

The downstream workflow qualifies successful Core CI **pushes to `master` in this
repository**. Pull-request, manual, scheduled, unsuccessful and foreign-repository
source runs are skipped. PRs still build, qualify and package their evaluated merge
commit in Core CI; integrated operations qualification starts after the master
push succeeds. This event boundary avoids confusing a PR branch head with its
built merge commit.

For a qualifying master push, checkout, artifact selection and evidence validation
all use that source run's `head_sha`, and the download uses its exact `run-id`.
Production-operations evidence retains and validates the producer workflow run ID.
A same-commit artifact from another run cannot substitute for the selected run.

## Complete readiness and inherited freshness

Packaging requires readiness schema 2 with `contract_version = 1`, environment
`ci_release_gate`, the expected commit/run, all five distinct required inputs, all
successful named checks and an empty `violations` array. It independently rechecks
input status, schema, environment, source identity, operational cycle completion,
research reproduction and qualification completeness. Invalid input produces a
structured FAIL RC report and summary before an archive is created.

The schema-1 RC report includes a required `release_evidence` record containing
`contract_version = 1`, `max_input_age_hours = 336`, the original readiness creation
time and SHA-256, source workflow run, `oldest_evidence_at`, and the full input
identity/timestamp records. The readiness digest must match
`manifest.release_readiness_sha256`. The archive preserves the original readiness
file byte-for-byte.

The 14-day age limit applies to every original input. Readiness and packaging
also reject missing, timezone-less, invalid or future timestamps and reversed
observation windows. RC creation has its own timestamp, but a new wrapper cannot
refresh the original evidence age. The handoff validator, Production Operations
and Consolidated Readiness recheck inherited provenance and freshness at their
own evaluation time. RC reports lacking this provenance cannot pass the current
contract; retain them for history and obtain a new Core CI result.

The outer RC report has a 720-hour age limit aligned with 30-day archive retention;
the original inputs remain subject to the stricter 336-hour limit. Local reports
may retain an explicit `unknown` source run, but cannot satisfy a consumer bound
to a known CI run.

The following are deliberately excluded:

- `config/config.json`;
- API keys, credentials or secrets;
- runtime databases;
- logs;
- machine-specific state.

## Pre-deployment checklist

Before using an RC in an environment:

1. Verify the external archive SHA-256 against the published `.sha256` file.
2. Extract into a new versioned directory; never overwrite the currently active binary in place.
3. Verify `SHA256SUMS` inside the bundle.
4. Confirm `MANIFEST.json.git_sha` matches the intended commit.
5. Confirm `evidence/release_readiness.json` uses schema version 2 and contract version 1, reports `PASS` with no violations, matches the manifest Git SHA/source run, and contains all required successful checks and original input identities/timestamps within their validity period.
6. Copy or generate environment configuration separately. Do not reuse unknown local configuration blindly.
7. Validate filesystem permissions and ensure secrets remain outside the RC directory.
8. Start in the lowest-risk supported mode appropriate for the environment; do not infer live-trading approval from RC status.
9. Verify health, market connectivity, persistence pressure and operator safety state before relying on runtime output.

## Recovery and rollback checklist

Rollback must remain possible without rebuilding the failed RC.

Before activation:

- retain the previously accepted binary and its configuration snapshot;
- record the previous commit/version identifier;
- preserve required database backup/recovery material according to the environment's persistence policy;
- ensure the operator knows how to stop Sentum cleanly and where shutdown progress is observed.

Trigger rollback or halt evaluation when any of the following occurs:

- startup does not reach the expected healthy operational state;
- kill switch becomes active unexpectedly;
- reconciliation is incomplete where reconciliation is required;
- market data remains disconnected;
- persistence pressure becomes critical/saturated and does not recover;
- shutdown cannot complete within the established operational budget;
- observed behavior conflicts with release-readiness evidence or the package manifest.

Rollback sequence:

1. Stop new work and preserve fail-closed behavior.
2. Request normal shutdown and observe shutdown progress to completion.
3. Do not clear a kill switch merely to make rollback proceed.
4. Preserve logs, runtime state and failure evidence before replacing files.
5. Restore the previous accepted binary and its matching environment configuration.
6. Perform required persistence/reconciliation checks before resuming operation.
7. Verify health and operator safety state after recovery.
8. Record the failed RC commit, symptoms and recovered version.

## Handoff record

For each RC handoff, retain at minimum:

- Git commit SHA;
- RC archive filename;
- archive SHA-256;
- release-readiness evidence;
- CI workflow run ID;
- target environment;
- operator/reviewer performing the handoff;
- deployment result: accepted, rejected or rolled back;
- rollback/recovery notes when applicable.

## Authority boundaries

The package and its evidence are downstream of runtime truth. They do not:

- synthesize fills;
- modify Risk or Execution semantics;
- clear kill switches;
- authorize live trading;
- replace exchange reconciliation;
- replace environment-specific operational approval.
