# Production Notification Operations Acceptance

Notification operations are accepted only when the final pull-request head satisfies this contract and Core CI succeeds for that exact head.

## Required capabilities

- Notification delivery operations are derived from append-only notification delivery evidence.
- Only the latest record per generation-safe delivery dedup key contributes to current-state counts.
- Pending, dispatched, delivered, failed, retriable-failed, terminal-failed and backlog counts are observable.
- Channel operations are deterministically ordered and expose active, delivered and failed counts.
- Health is one of `NO_ACTIVITY`, `HEALTHY`, `ATTENTION`, `INCIDENT_CANDIDATE`, or `UNAVAILABLE`.
- Missing delivery evidence fails closed as `UNAVAILABLE`.
- Terminal delivery failure may produce an `INCIDENT_CANDIDATE` signal, but the observability layer cannot create, acknowledge, resolve or mutate an incident.
- `/api/operations` exposes the same notification-operations projection used by the operator presentation contract.
- The web Operations surface renders notification health, backlog, terminal failures, incident candidate and per-channel state from `/api/operations` only.
- The dashboard remains read-only and exposes no notification or incident write route.

## Authority boundaries

The notification-operations projection is advisory and read-only.

It must keep:

- `incident_authorized = false`;
- `execution_authorized = false`.

It must not call notification providers, acknowledge or resolve alerts, clear kill switches, resume entries, approve governed actions, mutate Risk or Execution, synthesize fills, or override exchange-confirmed execution truth.

## Required regression evidence

The final head must build and run the notification operations regression together with the existing cross-surface/dashboard regressions. The notification operations regression must remain attached to the sanitizer build graph so ASan, UBSan and TSan do not register an unbuilt test executable.

Required scenarios include:

1. empty but available evidence -> `NO_ACTIVITY`;
2. append-only state evolution -> only latest state counted;
3. retriable failure -> `ATTENTION`;
4. terminal failure -> advisory `INCIDENT_CANDIDATE` only;
5. backlog threshold -> `ATTENTION`;
6. deterministic channel ordering;
7. missing evidence -> `UNAVAILABLE`;
8. dashboard consumes `notification_operations` without write methods;
9. cross-surface contract preserves `incident_authorized = false` and `execution_authorized = false`.

## Final-head rule

Earlier successful workflow runs do not establish notification-operations acceptance. Acceptance requires successful Core CI for the exact final PR head after this contract and all notification-operations functional changes are present. Until that evidence exists, the PR remains Draft and notification-operations status is `IMPLEMENTED / CI PENDING`.
