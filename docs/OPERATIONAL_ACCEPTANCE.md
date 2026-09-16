# Operational acceptance and soak evidence

AP-09 adds a repeatable operational acceptance layer on top of Sentum's existing lifecycle and observability regression tests.

## Purpose

The acceptance runner is intended to catch failures that only become visible across repeated start/stop, persistence-drain, queue-observability and recovery cycles. It does not replace focused unit or sanitizer coverage.

## Acceptance runner

`tools/ci/operational_acceptance.py` executes the existing Release binaries repeatedly:

- `sentum_runtime_lifecycle_tests`
- `sentum_runtime_observability_tests`

The lifecycle suite already exercises concurrent logger shutdown, non-empty persistence queue draining, collector shutdown under mock traffic, repeated dashboard start/stop, and recovery after partial dashboard initialization failure.

The observability suite covers queue depth/high-water accounting, pressure transitions, saturation accounting and sampled runtime metrics.

## CI contract

Pull-request Release CI runs five acceptance cycles. Every cycle must complete both binaries successfully. A non-zero exit code or per-check timeout fails the acceptance step.

Current CI defaults:

- cycles: `5`
- per-check timeout: `90s`
- report: `log/operational_acceptance.json`
- human summary: `log/operational_acceptance.md`
- evidence retention: `14 days`

The JSON report records each command, return code, elapsed time, stdout and stderr. The Markdown report is added to the GitHub Actions job summary.

## Evidence semantics

`PASS` means all requested cycles completed and every invoked regression binary returned success within its timeout.

`FAIL` means at least one cycle timed out or returned a non-zero status. The runner stops after the first failing cycle so the evidence remains focused and CI time is bounded.

This is an operational acceptance gate, not a production latency SLA. Performance budgets remain owned by AP-02 and their dedicated benchmark gate.

## Scope boundaries

AP-09 does not alter Strategy, Risk, Execution, Position/Trade State or fill truth. It does not synthesize exchange state and does not weaken fail-closed Testnet behavior.

Longer-duration soak runs, memory/RSS trend capture and fault-injection scenarios can be layered on this evidence contract without changing the runtime architecture.
