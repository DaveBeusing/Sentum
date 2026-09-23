# Governed Notification Delivery & Escalation Routing Acceptance

## Purpose

Notification delivery is accepted only when notification routing, delivery attempts, retry/idempotency semantics, provider boundaries and delivery evidence remain governed, bounded and isolated from trading/execution authority.

Only Core CI evidence for the final pull-request head counts as final acceptance evidence.

## Routing acceptance

Required behavior:

- policy schema version `1`;
- authority `NOTIFICATION_ROUTING_ONLY`;
- default action `BLOCK`;
- only governed channels/audiences may become eligible;
- acknowledged or cleared alerts are not routed;
- dedup keys include alert id, generation, escalation level, channel and audience;
- unknown/incomplete routes fail closed;
- routing authority never implies trading/execution authority.

## Delivery state-machine acceptance

Required states are `PENDING`, `DISPATCHED`, `DELIVERED`, `FAILED`.

Required behavior:

- `DELIVERED` is terminal;
- exhausted retry budget is terminal;
- retry count is bounded;
- retry backoff is deterministic and capped at 120 seconds;
- the state machine does not sleep or implement an unbounded retry loop;
- ineligible intents fail closed;
- duplicate attempts for the same generation-safe dedup key are rejected while pending, dispatched, delivered or terminal;
- a later alert generation may produce a distinct delivery attempt.

## Provider-boundary acceptance

Provider adapters receive only notification transport context and `delivery_authorized`.

Provider results may only produce notification-delivery state/evidence. They must not:

- acknowledge or resolve alerts;
- clear kill switches;
- resume entries;
- approve governed actions;
- mutate Risk or Execution state;
- synthesize fills;
- override exchange-confirmed execution truth.

Every provider request/result path keeps `execution_authorized = false`.

## Delivery-evidence acceptance

Delivery evidence must preserve at least:

- dedup key;
- alert id/generation;
- channel/audience;
- delivery state and attempt;
- terminal flag;
- provider reference;
- failure code/reason;
- observation timestamp when supplied;
- `execution_authorized = false`.

The notification-delivery evidence contract is append-only at the API boundary: new evidence is appended and earlier records are not rewritten. Immediate duplicate evidence is rejected.

Durable storage, SLOs, metrics and incident integration are outside this contract and belong to the subsequent production-notification operations layer.

## Regression acceptance

Final-head Core CI must execute and pass at least:

- `notification_delivery_state_machine`;
- `notification_provider_boundary`;
- existing operator-alerting regressions;
- existing lifecycle/regression suite;
- ASan;
- UBSan;
- TSan.

## Acceptance state

Notification delivery acceptance status is one of:

- `IMPLEMENTED / CI PENDING` — functional scope complete but final-head CI has not succeeded;
- `ACCEPTED` — final-head Core CI including required regressions and sanitizers completed successfully;
- `BLOCKED` — final-head CI or required acceptance regression failed.

Mergeable state or an earlier green run does not substitute for final-head CI evidence.
