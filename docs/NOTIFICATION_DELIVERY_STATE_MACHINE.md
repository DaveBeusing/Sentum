# Notification Delivery State Machine

AP-20 adds a deterministic, bounded delivery state machine downstream of governed notification routing.

## States

Delivery attempts use the following presentation/control states:

- `PENDING` — an eligible governed routing intent has produced one delivery attempt;
- `DISPATCHED` — one bounded provider send attempt has been started;
- `DELIVERED` — provider delivery was confirmed; this state is terminal;
- `FAILED` — the most recent dispatch failed. The state is retryable only while the configured attempt budget remains.

## Retry and backoff

Retries are bounded by `max_attempts` and use deterministic backoff:

- attempt 1: 5 seconds;
- attempt 2: 10 seconds;
- subsequent retries increase exponentially;
- backoff is capped at 120 seconds;
- once the attempt budget is exhausted, `FAILED` becomes terminal and no further retry is allowed.

The state machine does not sleep, poll providers or run an unbounded retry loop. It only derives the next permitted delivery state and exposes the required backoff.

## Idempotency

Every governed routing intent carries a generation-safe deduplication key containing alert id, alert generation, escalation level, channel and audience.

A delivery attempt is not created again when the same key already has `PENDING`, `DISPATCHED`, `DELIVERED` or terminal failure evidence. A later alert generation receives a different key and may produce a new attempt.

This prevents repeated rendering/evaluation of the same snapshot from creating duplicate deliveries.

## Fail-closed boundary

A blocked or otherwise ineligible routing intent immediately produces a terminal `FAILED / NOT_ELIGIBLE` attempt if passed to the state-machine boundary.

Delivery authorization means notification transport only. It never authorizes trading or operational state changes. Every state keeps `execution_authorized = false`.

The notification path must not acknowledge or resolve alerts, clear a kill switch, resume entries, approve governed actions, mutate Risk/Execution, synthesize fills or override exchange-confirmed execution truth.

## Evidence

Provider references and failure code/reason are retained on the attempt model so later persistence/audit slices can store immutable delivery evidence without changing notification routing semantics.
