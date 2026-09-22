# Sentum Resilience & Autonomous Recovery Guardrails

## Purpose

This document defines the AP-15 resilience baseline above the existing production-readiness, continuous-readiness and reliability evidence chain.

The objective is controlled recovery, not autonomous trading authority.

## Evidence boundary

CI evidence produced by this package uses `environment_class = ci_rehearsal` and validates repository policy behavior only.

It is not proof that a real production incident, failover, restart or recovery action occurred.

## Circuit breaker states

The resilience layer consumes existing reliability states:

- `READY`: observe; no recovery action required.
- `ATTENTION`: enter degraded operating posture and escalate according to the runbook.
- `BLOCKED`: trip the circuit breaker and keep trading fail-closed.

A tripped breaker may reset only after the configured number of consecutive `READY` samples. Any `ATTENTION` or `BLOCKED` sample resets that readiness sequence.

## Allowed autonomous actions

Autonomous actions are limited to non-authoritative operational maintenance:

- restart a non-authoritative worker;
- reopen a read-only monitoring channel;
- rebuild an ephemeral cache;
- rotate a local log sink;
- revalidate a recovery artifact.

These actions must not create or alter execution truth.

## Operator approval boundary

The following actions require explicit operator approval and separate authoritative validation:

- resume entries;
- clear a kill switch;
- promote a recovery candidate;
- accept a reconciliation result.

## Forbidden autonomous actions

Automation must never autonomously:

- clear a kill switch;
- enable entries;
- synthesize fills;
- mutate Risk state;
- mutate Execution state;
- override exchange-confirmed execution truth.

If a required recovery would cross one of these boundaries, automation must stop and escalate.

## Recovery sequence

The governed recovery sequence is:

1. Detect ATTENTION or BLOCKED from existing monitoring/reliability evidence.
2. Preserve diagnostic evidence.
3. Trip or maintain the circuit breaker when required.
4. Execute only allow-listed non-authoritative recovery actions.
5. Re-run health/readiness/reliability checks.
6. Require the configured consecutive READY samples before breaker reset is considered.
7. Require explicit operator approval for any action that can resume trading or alter authoritative acceptance.
8. Reconcile authoritative external state before entries can resume where applicable.

## Incident recovery lifecycle

Incident acknowledgement and incident recovery are separate durable states. Acknowledgement alone does not imply recovery, reconciliation acceptance, kill-switch clearing or entry resumption.

The incident lifecycle may enter `RECOVERY_IN_PROGRESS` only after acknowledgement and only with explicit reconciliation evidence. Resolving or closing the incident records operational state only; it does not perform or authorize any trading recovery action.

## Fail-closed requirements

Missing, stale, mismatched or failed upstream evidence produces FAIL.

A resilience PASS does not authorize live trading. It proves only that the recovery policy and guardrails are internally consistent for the evaluated commit and environment class.
