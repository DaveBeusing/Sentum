# Sentum Release Readiness Contract

Copyright (C) 2026 Dave Beusing <david.beusing@gmail.com>
SPDX-License-Identifier: MIT

## Principle

Release readiness is evidence-based and fail-closed. A successful build alone is not sufficient.

The Release Readiness gate is downstream of the Release build/regression job and the complete ASan, UBSan and TSan matrix. It consumes immutable evidence produced by the same Core CI run for the exact commit under evaluation.

Release Readiness is repository evidence. It is not target-environment production acceptance and does not authorize production-money trading.

## Required evidence

The gate requires all of the following for the exact same Git commit:

- `performance_gate.json`, schema version 2, `status = PASS`, environment class `ci_hosted_runner`;
- `operational_acceptance.json`, schema version 1, `status = PASS`, environment class `ci_rehearsal`, with every requested cycle completed;
- `paper-soak.json`, runtime qualification schema version 1, `status = PASS`, `evidence_complete = true`, environment class `ci_rehearsal`;
- `all-faults.json`, runtime qualification schema version 1, `status = PASS`, `evidence_complete = true`, environment class `ci_rehearsal`;
- `research_validation.json`, independent research validation schema version 1, `status = PASS`, environment class `research_validation`, with canonical deterministic reproduction `PASS`.

Every report must identify the same `git_sha` as the commit under evaluation and
carry the same `workflow_run_id` as the consuming Core CI run. Evidence from another
commit, run or environment class is rejected. Research may identify its commit in
`code_identity.git_commit`; when both identities are present they must agree.

Qualification is the exact named set `paper-soak` and `all-faults`, once each. The
`scenario` field is authoritative regardless of filename or command-line order.
Missing names, duplicates, unknown substitutions and additional scenarios fail the
gate; a filename is never substituted for a missing scenario field.

The release-readiness report records each input's path, SHA-256, schema, status,
commit, source run, environment and original timestamps. Required input semantics
are retained as well: completed/requested operational cycles, successful research
reproduction and complete qualification evidence.

## Evidence age

Each original release input has a maximum age of **336 hours (14 days)**, matching
the Core CI input-artifact retention period. These source fields are mandatory:

| Input | Original timestamp fields |
| --- | --- |
| Performance | `generated_at` |
| Operational acceptance | `generated_at` |
| Independent research validation | `validated_at_utc` |
| Each runtime qualification | `started_at_utc` and `ended_at_utc` |

Timestamps require ISO-8601 with an explicit UTC offset and are normalized to UTC
for comparison. Missing, malformed, timezone-less, future or expired timestamps
fail validation. A qualification window must be ordered; its start is the age
anchor so a freshly completed long run cannot hide old observations. An input
cannot postdate the readiness report that consumes it. Exactly 336 hours is
accepted; any older input is rejected.

Each input retains the original `timestamps` map and its oldest `observed_at`.
`oldest_evidence_at` is the oldest timestamp across all five inputs. It is carried
through the RC handoff and checked again by downstream consumers; composing or
repackaging a report cannot restart this validity period.

## CI dependency contract

The `release_readiness` job declares explicit dependencies on:

- the Release build job, including regression tests;
- Operational Acceptance;
- Core CI Runtime Qualification smoke;
- enforced performance regression budgets;
- canonical independent research validation;
- CLI, research and dashboard smoke coverage;
- the sanitizer matrix covering ASan, UBSan and TSan.

If any upstream job fails or does not complete successfully, the Release Readiness job cannot produce a PASS result.

## Output contract

The gate emits:

- `log/release_readiness.json`, schema version 2 with required `contract_version = 1`;
- `log/release_readiness.md` for the CI summary.

The JSON report contains:

- `generated_at`;
- `environment_class = ci_release_gate`;
- exact `git_sha`;
- source workflow run identifier;
- SHA-256 and identity metadata for every consumed evidence report;
- all required named checks exactly once, each with `status = PASS`;
- `violations = []` for a passing result;
- `max_input_age_hours = 336` and the original `oldest_evidence_at`;
- limitations describing what PASS does not prove.

The readiness artifact is retained for 30 days and becomes an immutable input to
RC packaging. Retention is not a validity extension: its source observations
still expire after 14 days. A malformed or missing input writes a structured FAIL
report and summary so the failed gate retains useful evidence.

The packager independently validates this complete contract, including source
records, named checks, empty violations and freshness. A two-field PASS/SHA object
or an older schema-2 report without `contract_version = 1` is insufficient. Older
artifacts remain historical evidence; run Core CI again to obtain the current
contract instead of editing old PASS reports.

Local tooling may consistently use `workflow_run_id = unknown` when no CI run ID
is available. This provenance remains explicit and is never promoted to a known
run: a consumer supplied with `GITHUB_RUN_ID` or `SOURCE_WORKFLOW_RUN_ID` requires
that exact identity. Local unknown evidence cannot satisfy a CI-bound handoff.

## PASS meaning

A PASS means that the repository-defined, release-blocking evidence chain for the evaluated commit completed successfully.

It proves neither:

- target-environment deployment acceptance;
- exchange availability;
- production latency or memory SLAs;
- future profitability;
- permission to promote a model automatically;
- permission to route production-money orders.

## Release-blocking conditions

A commit is not release-ready when any of the following is true:

- Release build or regression tests fail;
- ASan, UBSan or TSan fails;
- Operational Acceptance fails, times out or completes fewer cycles than requested;
- enforced performance budgets fail;
- Core CI Runtime Qualification evidence is missing, incomplete, failing, commit-mismatched or environment-mismatched;
- canonical Independent Research Validation is missing, failing, commit-mismatched, environment-mismatched or lacks a successful deterministic reproduction comparison;
- required evidence is missing, malformed, expired, future-dated, from another source run, or has an unsupported schema/contract;
- the final Release Readiness gate does not report PASS.

## Extended qualification

The scheduled/manual `Sentum Runtime Qualification` workflow remains extended qualification evidence rather than a per-pull-request gate.

Its longer soak and individual fault runs are intentionally classified as scheduled advisory evidence. They must not be silently substituted for Core CI evidence, and they must not be presented as target-environment acceptance.

If extended qualification is promoted to a release-blocking requirement in the future, Release Readiness must consume immutable same-commit evidence from the completed qualification run rather than rerunning an uncontrolled workload during deployment.

## Operational use

The Release Readiness artifact is consumed by the deterministic RC packager. The tested and approved repository evidence therefore remains bound to the same Git commit that is packaged.

Human operational approval and target-environment acceptance remain separate from this automated gate. See [Readiness Evidence Contract](READINESS_EVIDENCE.md), [RC Handoff](RC_HANDOFF.md) and [Production Validation](PRODUCTION_VALIDATION.md).
