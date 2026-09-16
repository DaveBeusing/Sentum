# Sentum release readiness contract

AP-10 defines the repository-level evidence required before a commit may be treated as release-ready.

## Principle

Release readiness is evidence-based and fail-closed. A successful build alone is not sufficient. The final readiness gate is downstream of the Release build/regression job and the complete ASan, UBSan and TSan matrix.

## Required evidence

The final gate requires both machine-readable reports to exist for the same commit:

- `performance_gate.json` with schema version 1 and `status = PASS`;
- `operational_acceptance.json` with schema version 1, `status = PASS`, and all requested acceptance cycles completed.

Both reports must contain a `git_sha` equal to the commit under evaluation. Evidence from another commit is rejected.

## CI dependency contract

`release_readiness` declares explicit dependencies on:

- the Release build job, including regression tests, operational acceptance, CLI smoke, AP-02 performance budgets, research smoke and dashboard smoke;
- the sanitizer matrix covering ASan, UBSan and TSan.

If any upstream job fails or does not complete successfully, the final readiness job cannot produce a PASS result.

## Output

The gate emits:

- `log/release_readiness.json` for machine-readable evidence;
- `log/release_readiness.md` for the CI summary.

The readiness artifact is retained for 30 days.

## PASS meaning

A PASS means only that the repository-defined readiness gates for the evaluated commit completed successfully. It does not certify exchange availability, external infrastructure, market conditions, profitability, or production behavior outside the tested scope.

## Release-blocking conditions

A commit is not release-ready when any of the following is true:

- Release build or regression tests fail;
- operational acceptance fails, times out, or completes fewer cycles than requested;
- AP-02 performance budgets fail;
- ASan, UBSan or TSan fails;
- required evidence is missing, malformed, has an unsupported schema, or belongs to another commit;
- the final release-readiness gate does not report PASS.

## Operational use

The release-readiness artifact should be attached to or referenced from release-candidate review. Human release approval remains separate from the automated gate; the automation supplies evidence and prevents unsupported green claims.
