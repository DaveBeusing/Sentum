# Operational acceptance and soak evidence

Sentum uses a repeatable operational acceptance layer on top of the existing lifecycle and observability regression tests.

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

Timeouts produce a structured `FAIL` report with return code `124`,
`timed_out = true`, actual elapsed time and all output captured before the deadline.
Both successful output and timeout byte buffers are decoded as UTF-8; invalid or
incomplete sequences use the Unicode replacement character. Empty stdout/stderr
remain empty strings, while the separate `failure` field explains the timeout.
The Markdown failure section includes the command, duration, timeout state and
captured output, so both artifact formats remain available when acceptance fails.
Each command owns a separate process group; timeout cleanup kills that group,
including descendants retaining output pipes, before the runner continues.

The runner's timeout/report regressions can be run without C++ build artifacts:

```bash
python3 tests/subprocess_evidence_tests.py
```

## Evidence semantics

`PASS` means all requested cycles completed and every invoked regression binary returned success within its timeout.

`FAIL` means at least one cycle timed out or returned a non-zero status. The runner stops after the first failing cycle so the evidence remains focused and CI time is bounded.

This is an operational acceptance gate, not a production latency SLA. Performance budgets remain owned by the dedicated benchmark gate.

## Scope boundaries

Operational acceptance does not alter Strategy, Risk, Execution, Position/Trade State or fill truth. It does not synthesize exchange state and does not weaken fail-closed Testnet behavior.

Longer-duration soak runs, memory/RSS trend capture and deterministic fault injection are implemented by the runtime qualification layer documented in [RUNTIME_QUALIFICATION.md](RUNTIME_QUALIFICATION.md).

Core CI keeps operational acceptance and runtime qualification as separate evidence artifacts. The acceptance runner remains the repeated lifecycle/observability gate; runtime qualification adds seeded Paper duration, Testnet/recovery fault scenarios and memory trend evidence without changing the runtime authority model.
