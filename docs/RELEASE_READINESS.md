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

Every report must identify the same `git_sha` as the commit under evaluation. Evidence from another commit or the wrong environment class is rejected.

The release-readiness report records the SHA-256 digest of every consumed evidence file so the exact inputs to the decision remain identifiable.

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

- `log/release_readiness.json`, schema version 2;
- `log/release_readiness.md` for the CI summary.

The JSON report contains:

- `generated_at`;
- `environment_class = ci_release_gate`;
- exact `git_sha`;
- source workflow run identifier;
- SHA-256 and identity metadata for every consumed evidence report;
- explicit individual checks;
- limitations describing what PASS does not prove.

The readiness artifact is retained for 30 days and becomes an immutable input to RC packaging.

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
- required evidence is missing, malformed or has an unsupported schema;
- the final Release Readiness gate does not report PASS.

## Extended qualification

The scheduled/manual `Sentum Runtime Qualification` workflow remains extended qualification evidence rather than a per-pull-request gate.

Its longer soak and individual fault runs are intentionally classified as scheduled advisory evidence. They must not be silently substituted for Core CI evidence, and they must not be presented as target-environment acceptance.

If extended qualification is promoted to a release-blocking requirement in the future, Release Readiness must consume immutable same-commit evidence from the completed qualification run rather than rerunning an uncontrolled workload during deployment.

## Operational use

The Release Readiness artifact is consumed by the deterministic RC packager. The tested and approved repository evidence therefore remains bound to the same Git commit that is packaged.

Human operational approval and target-environment acceptance remain separate from this automated gate. See [Readiness Evidence Contract](READINESS_EVIDENCE.md), [RC Handoff](RC_HANDOFF.md) and [Production Validation](PRODUCTION_VALIDATION.md).
