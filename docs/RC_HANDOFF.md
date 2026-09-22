# Sentum Release Candidate Handoff

## Purpose

This document defines the handoff contract for a Sentum release candidate after the repository release-readiness gate has passed.

The RC package is an immutable operational handoff artifact. It is not permission to enable live trading. Exchange credentials, environment-specific configuration and operational approval remain outside the package.

## Required RC evidence

An RC is eligible for handoff only when all of the following belong to the exact same Git commit:

- Release build and regression tests passed;
- AP-02 performance gate passed;
- AP-09 operational acceptance passed with all requested cycles completed;
- ASan passed;
- UBSan passed;
- TSan passed;
- AP-10 release-readiness gate passed;
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

Before the production-operations gate executes, `tools/ci/validate_rc_handoff.py` verifies the exact directory structure, report schema and PASS state, full Git SHA, expected archive path, archive SHA-256 and checksum-file contents. Missing, malformed or commit-mismatched evidence is a hard failure; alternate fallback locations are not accepted.

The downstream workflow selects the artifact by the completed Core CI run's `head_sha` and `run-id`. Production-operations evidence retains that source workflow run ID so reports remain traceable to the exact Core CI execution that produced the RC.

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
5. Confirm `evidence/release_readiness.json.status` is `PASS` and its `git_sha` matches the manifest.
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
