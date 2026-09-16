# Operator Alert Escalation Timeline

AP-19 adds a read-only escalation and attention-aging projection for operator alerts.

## Evidence boundary

The presentation layer does not read the wall clock and does not calculate escalation deadlines from local policy. Aging is derived only from timestamp evidence already supplied by the operations control plane:

- `operations_control_plane.observed_at_utc`
- `operations_control_plane.alert_escalation[].first_seen_utc`
- `last_seen_utc`
- `next_escalation_at_utc`
- optional `timeline[]` evidence with state, actor and reason

Canonical UTC evidence uses ISO-8601 `...Z` timestamps. Missing or malformed evidence produces `AGING UNAVAILABLE`; the UI does not guess an age or deadline.

## Presentation states

Active, unacknowledged alerts with valid evidence can be shown as `FRESH`, `DUE` or `OVERDUE`. `ACKNOWLEDGED` and `CLEARED` lifecycle states override aging so resolved operator attention is not presented as overdue.

The timeline is bounded for terminal and web rendering. Counts remain available when rows are truncated.

## Cross-surface contract

Terminal renders the same escalation timeline projection that `/api/operations` exposes as `alert_escalation`. The contract contains total, due, overdue and unavailable counts plus bounded evidence rows.

## Authority

This slice is presentation-only. It does not advance escalation levels, send notifications, create or acknowledge requests, resolve alerts, clear kill switches, resume entries, mutate Risk/Execution, create fills or override exchange-confirmed execution truth. Every escalation row and view keeps `execution_authorized = false`.
