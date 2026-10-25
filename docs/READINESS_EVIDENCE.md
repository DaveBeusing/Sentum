# Readiness Evidence Contract

Copyright (C) 2026 Dave Beusing <david.beusing@gmail.com>
SPDX-License-Identifier: MIT

## Purpose

This document defines the authoritative hierarchy for Sentum build, test, sanitizer, performance, operational, qualification, research-validation and target-environment evidence.

The contract is deliberately conservative:

- repository CI may establish `REPOSITORY_READY`;
- only an independently supplied target-environment acceptance record may establish `TARGET_ACCEPTED`;
- neither state authorizes production-money trading;
- missing, stale, commit-mismatched or environment-mismatched required evidence fails closed.

## Evidence dependency graph

```text
source Git SHA
  |
  +-> Release build + regression tests
  |
  +-> ASan / UBSan / TSan
  |
  +-> Operational Acceptance -------------------+
  |                                             |
  +-> Runtime Qualification smoke --------------+
  |                                             |
  +-> Performance Gate -------------------------+--> Release Readiness
  |                                             |        |
  +-> Independent Research Validation ----------+        v
  |                                                  RC binary + package
  |                                                        |
  |                                                        v
  |                                              Production Operations Gate
  |                                                        |
  |                                  +---------------------+---------------------+
  |                                  |                     |                     |
  |                                  v                     v                     v
  |                         Operational rehearsal   Continuous readiness   Reliability
  |                                                        |                     |
  |                                                        +----------+----------+
  |                                                                   v
  |                                                            Resilience gate
  |                                                                   |
  |                                                                   v
  |                                                     Operations control plane
  |                                                                   |
  +-------------------------------------------------------------------+
                                                                      v
                                                        Consolidated Readiness
                                                                      |
                                                        REPOSITORY_READY
                                                                      |
                                         target-environment acceptance record
                                                                      |
                                                                      v
                                                               TARGET_ACCEPTED
```

Later gates consume earlier immutable evidence rather than recreating its authority. The RC package binds the exact release-readiness file by SHA-256, the Production Operations Gate records the exact RC-report SHA-256 it consumed, and the reliability -> resilience -> operations-control-plane chain records the SHA-256 of each direct upstream evidence file. Consolidated Readiness verifies those links in addition to commit, schema, environment and status.

## Evidence catalog

| Evidence | Producer | Schema | Git binding | Artifact binding | Environment class | Retention / expiry | Consumer | PASS means | PASS does not prove |
| --- | --- | ---: | --- | --- | --- | --- | --- | --- | --- |
| Performance gate | `tools/ci/performance_gate.py` | 2 | `git_sha` | report SHA-256 in Release Readiness | `ci_hosted_runner` | maximum age 336 hours; CI artifact 14 days; wrong SHA/run invalid immediately | Release Readiness | enforced CI performance/correctness budgets passed | production latency, memory or SLA behavior |
| Operational acceptance | `tools/ci/operational_acceptance.py` | 1 | `git_sha` | report SHA-256 in Release Readiness | `ci_rehearsal` | maximum age 336 hours; CI artifact 14 days; wrong SHA/run invalid immediately | Release Readiness | repeated lifecycle/observability cycles passed | target deployment acceptance |
| Runtime qualification smoke | `tools/ci/runtime_qualification.py` | 1 | `git_sha` | report SHA-256 in Release Readiness | `ci_rehearsal` | maximum age 336 hours; CI artifact 14 days; wrong SHA/run invalid immediately | Performance + Release Readiness | bounded Paper/fault qualification evidence is complete | long-duration target behavior |
| Independent research validation | `tools/verify_research.py` | 1 | `git_sha` / persisted code identity | report SHA-256 in Release Readiness | `research_validation` | maximum age 336 hours; CI artifact 14 days; wrong SHA/run invalid immediately | Release Readiness | persisted research provenance, boundaries, metrics and canonical reproduction validated | profitability, model promotion or execution readiness |
| Release Readiness | `tools/ci/release_readiness.py` | 2, contract 1 | `git_sha` | SHA-256 of every consumed evidence report | `ci_release_gate` | source age maximum 336 hours; CI artifact 30 days; wrong SHA/run invalid immediately | RC packager | all required repository release evidence for the exact commit passed | target-environment acceptance |
| RC package | `tools/ci/package_release_candidate.py` | 1 | `git_sha` | binary, release-readiness and archive SHA-256 | `ci_artifact` | original inputs maximum 336 hours; wrapper/retention maximum 720 hours | Production Operations + Consolidated Readiness | immutable handoff artifact was produced from approved evidence | deployment or runtime acceptance |
| Production Operations Gate | `tools/ci/production_operations_gate.py` | 1 | `git_sha` | RC report SHA-256 | `ci_rehearsal` | consolidated policy maximum 720 hours | Consolidated Readiness | RC identity and required operations contracts passed repository validation | real deployment execution |
| Operational validation rehearsal | `tools/ci/operational_validation_rehearsal.py` | 1 | `git_sha` | report SHA-256 in consolidated result | `ci_rehearsal` | consolidated policy maximum 720 hours | Consolidated Readiness | controlled backup/rollback/incident rehearsal passed | actual target backup, rollback or incident exercise |
| Continuous readiness rehearsal | `tools/ci/continuous_readiness_rehearsal.py` | 1 | `git_sha` | report SHA-256 in consolidated result | `ci_rehearsal` | consolidated policy maximum 720 hours | Reliability + Consolidated Readiness | readiness classification policy behaves as expected | live observation window |
| Reliability gate | `tools/ci/reliability_gate.py` | 1 | `git_sha` | exact Continuous Readiness SHA-256 plus report SHA-256 in consolidated result | `ci_rehearsal` | consolidated policy maximum 720 hours | Resilience + Consolidated Readiness | repository reliability policy passed | measured production availability |
| Resilience gate | `tools/ci/resilience_guardrail_gate.py` | 1 | `git_sha` | exact Reliability SHA-256 plus report SHA-256 in consolidated result | `ci_rehearsal` | consolidated policy maximum 720 hours | Operations control plane + Consolidated Readiness | recovery guardrail policy passed | autonomous execution authority |
| Operations control plane | `tools/ci/operations_control_plane_gate.py` | 1 | `git_sha` | exact Resilience SHA-256 plus report SHA-256 in consolidated result | `ci_rehearsal` | consolidated policy maximum 720 hours | Consolidated Readiness | action-classification governance passed | permission to trade |
| Consolidated Readiness | `tools/ci/consolidated_readiness.py` | 1 | `git_sha` | SHA-256 for every underlying evidence file | repository evidence set | regenerated per operations run | operator / release review | exact-commit repository evidence is complete | target-environment acceptance unless separately supplied |
| Target-environment acceptance | deployment operator record | 1 | `git_sha` | deployed `artifact_sha256` | `target_environment` | maximum 24 hours for a consolidated acceptance evaluation | Consolidated Readiness | the exact artifact was accepted in the named target environment for the recorded observation window | production-money trading authorization or profitability |

## Blocking classes

The repository uses three different evidence classes rather than forcing every qualification workload into every pull request:

- **release-blocking:** Release build/regression tests, sanitizer matrix, Operational Acceptance, enforced Performance Gate, Core CI Runtime Qualification smoke and canonical Independent Research Validation;
- **scheduled advisory:** the extended Runtime Qualification workflow. It provides longer soak/fault evidence and remains visible as its own workflow without delaying unrelated pull-request feedback;
- **deployment-blocking:** target-environment acceptance. A production-like deployment claim is incomplete without this separate record.

A scheduled advisory failure must be investigated for the affected commit before relying on that evidence, but absence of an extended run is not converted into a repository Release Readiness PASS or FAIL.

## Staleness and identity

Evidence is valid only for the Git SHA that produced it.

Original release inputs have a **336-hour (14-day)** maximum age, aligned with
Core CI input-artifact retention. The authoritative fields are performance and
operational `generated_at`, research `validated_at_utc`, and qualification
`started_at_utc`/`ended_at_utc`. Qualification windows must be ordered and their
start anchors observation age. All timestamps require an explicit UTC offset;
missing, malformed, timezone-less, future and expired values fail closed.

Release Readiness schema 2 requires contract version 1. Each source record retains
its exact identity, SHA-256, schema, status, environment, source workflow run and
original timestamps. The original oldest observation is propagated in
`oldest_evidence_at`. The RC report carries these records in `release_evidence`,
bound to the original readiness digest in its manifest. Packaging independently
validates all inputs and all named checks; handoff validation compares the snapshot
against the original readiness embedded in the archive.

Consolidated Readiness revalidates inherited RC provenance, including the required
five identities and original observation times. It displays RC age from
`release_evidence.oldest_evidence_at`, not its newest packaging timestamp. A fresh
wrapper around stale inputs remains blocked. When `SOURCE_WORKFLOW_RUN_ID` is
provided, the RC must come from that exact source run, and every inherited leaf
must carry the same identity.

The consolidated policy retains a 720-hour maximum for other repository
operational evidence and the RC wrapper itself, matching 30-day handoff retention.
This does not extend the stricter original-input validity period. Target timestamps
also require explicit offsets. The evaluator calculates each consumed report's
SHA-256 and verifies configured direct-upstream artifact links. Historical reports
without the required release provenance are not silently upgraded; a new Core CI
run produces current evidence.

Target-environment acceptance is evaluated with a 24-hour maximum age because it represents a concrete deployment observation rather than a reusable repository CI result. Its `artifact_sha256` must equal the `binary_sha256` recorded by the RC package report, so target acceptance cannot be attached to a different deployed binary.

These rules are fail-closed:

- another Git SHA is invalid;
- a missing required report is invalid;
- an unsupported schema is invalid;
- a non-PASS required repository report is invalid;
- a stale, future-dated or timezone-ambiguous report is invalid;
- repackaging cannot refresh the oldest original release observation;
- original-input, readiness and RC source workflow identities must agree;
- local `unknown` source identity cannot satisfy a known CI workflow run;
- missing, duplicated or substituted qualification identities are invalid;
- incomplete named readiness checks or a nonempty violations array are invalid;
- a configured upstream artifact-hash mismatch is invalid;
- a target observation window must use valid ISO-8601 timestamps, must not be reversed and must end no later than `validated_at_utc`;
- a `ci_rehearsal` record can never satisfy `target_environment` acceptance;
- missing target acceptance can never be synthesized into `TARGET_ACCEPTED`.

## Consolidated result states

### `BLOCKED`

At least one required repository evidence item is missing, stale, mismatched or failing, or a supplied target-acceptance record is invalid.

### `REPOSITORY_READY`

All required repository evidence for the exact commit is valid. No target-environment acceptance has been supplied.

This is the normal successful result of repository automation.

### `TARGET_ACCEPTED`

All required repository evidence is valid and a separate target-environment record also validates for the same Git SHA and deployed artifact.

This state still does not authorize production-money trading. Sentum's supported execution boundary remains Binance Spot Testnet.

## Target-environment acceptance record

Repository CI does not create this record. An operator may provide it to the consolidated evaluator after completing the documented deployment-validation procedure.

Minimum shape:

```json
{
  "schema_version": 1,
  "status": "ACCEPTED",
  "git_sha": "<full-git-sha>",
  "environment_class": "target_environment",
  "validated_at_utc": "2026-09-23T12:00:00+00:00",
  "artifact_sha256": "<deployed-binary-or-release-artifact-sha256>",
  "target_environment": "<environment-identifier>",
  "operator": "<operator-identifier>",
  "observation_window": {
    "start_utc": "2026-09-23T11:30:00+00:00",
    "end_utc": "2026-09-23T12:00:00+00:00"
  }
}
```

The compact record is accepted only when `artifact_sha256` matches the RC package's `binary_sha256`. Additional fields required by `PRODUCTION_VALIDATION.md` remain part of the operational handoff record even when this compact machine-readable acceptance record is used for consolidation.

## Automated versus manual release checks

Automated repository gates cover:

- build and regression tests;
- sanitizer matrix;
- Operational Acceptance;
- enforced performance budgets;
- Core CI runtime qualification;
- independent research evidence validation;
- release-readiness composition;
- deterministic RC packaging and checksums;
- operations/reliability/resilience/control-plane rehearsals;
- consolidated evidence identity and staleness validation.

Manual or target-specific work remains:

- selecting and identifying the target environment;
- supplying target credentials/configuration outside the RC bundle;
- pre-deployment backup/recovery-point ownership;
- deployment of the exact approved artifact;
- real runtime/market/persistence observation;
- exchange/account reconciliation where applicable;
- operator approval, rollback decisions and the final target-acceptance record.

## Extended runtime qualification

The scheduled/manual `Sentum Runtime Qualification` workflow remains advisory because it is intentionally longer-running and is not a stable per-PR latency or production-SLA measurement.

A future decision to make it release-blocking must reference immutable same-commit qualification evidence rather than rerunning an uncontrolled workload during deployment.
