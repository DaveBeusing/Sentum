# Operator Alerting, Notification & Escalation UX

## Purpose

AP-19 introduces a presentation-only alert model for operational conditions already present in Sentum runtime and control-plane truth.

This foundation does not create a notification transport, does not acknowledge or resolve incidents, and does not add execution authority. It translates existing runtime, market-data, persistence and governance state into deterministic operator alert records.

## Alert record

`OperatorAlertPolicy.hpp` emits alerts with:

- stable alert id;
- source domain;
- severity;
- title and message;
- operator guidance;
- recommended workspace;
- whether acknowledgement is required;
- escalation level;
- whether the condition is a candidate for external notification;
- `execution_authorized = false`.

The alert policy is deterministic and side-effect free.

## Severity and escalation

The initial presentation levels are:

- `INFO` / escalation level 0 — informational only;
- `ATTENTION` / level 1 — operator awareness, no acknowledgement required by default;
- `WARNING` / level 2 — acknowledgement required and eligible for notification;
- `CRITICAL` / level 3 — immediate operator attention, acknowledgement required and eligible for notification.

This alert policy only classifies presentation urgency and remains side-effect free. Governed eligible alerts can now be dispatched separately by `NotificationDispatchRuntime` when notification dispatch is explicitly enabled and a provider is configured for the routed channel. The alert policy itself still performs no transport.

## Initial alert sources

The first policy derives alerts from existing runtime truth:

- active kill switch;
- unhealthy runtime state;
- market-data disconnect;
- elevated / critical / saturated persistence pressure;
- unavailable governance state;
- stale governance evidence;
- active operational incident;
- pending governed approvals;
- entries paused.

Healthy runtime with controlled, available governance evidence produces no alert.

## Authority boundary

Alerts cannot:

- clear a kill switch;
- resume entries;
- approve a governed request;
- acknowledge or resolve an incident authoritatively;
- mutate maintenance or recovery state;
- mutate Risk or Execution state;
- create or alter fills;
- override exchange-confirmed execution truth.

`execution_authorized` is always false in alert presentation records.

Acknowledgement requirements remain presentation metadata. Notification providers cannot acknowledge or resolve alerts, and the UI must not invent successful acknowledgement locally.

## Fail-closed behavior

Missing control-plane governance is surfaced as a warning rather than treated as healthy.

Stale governance evidence is a warning and explicitly instructs the operator not to rely on stale approval state.

Unknown alert severity values are not consumed from external input in this policy; severity is derived by code-owned rules from existing runtime truth.

## Regression coverage

`operator_alert_policy_tests` verifies:

- healthy state produces no alert;
- kill-switch alert is critical, acknowledgement-required and escalation level 3;
- runtime failures sort ahead of lower-severity alerts;
- market disconnect uses warning / level 2 semantics;
- stale and missing governance remain visible and fail closed;
- pending approvals and paused entries remain attention-only by default;
- active incidents require acknowledgement but do not grant local resolution authority;
- all alert records retain `execution_authorized = false`.

The test target is attached to the existing sanitizer dependency graph so ASan, UBSan and TSan build it before CTest execution.

## Notification dispatch integration

Routing, delivery state, durable evidence and provider execution are implemented outside the alert presentation policy. See `NOTIFICATION_DISPATCH_RUNTIME.md`. Notification delivery never changes alert lifecycle authority.
