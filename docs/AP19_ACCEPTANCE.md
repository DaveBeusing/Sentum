# AP-19 Acceptance — Operator Alerting, Notification & Escalation UX

## Purpose

AP-19 is accepted only when operator alert classification, lifecycle, acknowledgement presentation, noise control, cross-surface Alert Center rendering and escalation-aging presentation are all deterministic, read-only and regression-covered.

Only Core CI evidence for the final pull-request head counts as final acceptance evidence. Earlier successful runs are supporting baselines only.

## Alert classification acceptance

Alert severity and escalation presentation must remain code-owned and deterministic for the supported runtime/control-plane truth sources.

Required severity vocabulary:

- `INFO`;
- `ATTENTION`;
- `WARNING`;
- `CRITICAL`.

Every alert remains presentation-only with `execution_authorized = false`.

## Lifecycle acceptance

Alert lifecycle presentation must preserve:

- stable alert identifiers;
- `NEW`, `ACTIVE`, `ACKNOWLEDGED`, `CLEARED` states;
- stable-id deduplication across observations;
- generation increment on re-activation after clear;
- acknowledgement evidence matched to exact `alert_id + generation`;
- rejection of stale or malformed acknowledgement evidence;
- no locally invented acknowledgement, clear or resolution authority.

## Alert Center acceptance

Terminal and `/api/operations` must consume the same shared Alert Center projection.

Required shared presentation includes:

- active, acknowledged and cleared counts;
- severity counts;
- bounded alert rows;
- generation and lifecycle state;
- acknowledgement evidence when present;
- operator guidance and recommended workspace;
- `execution_authorized = false` throughout.

When persisted lifecycle history is absent, presentation may show current active alerts but must not invent cleared history.

## Noise-control acceptance

Operator noise control must remain presentation-only.

Required behavior:

- `CRITICAL` and `WARNING` are never suppressed;
- low-severity `ATTENTION` and `INFO` may use a bounded presentation budget;
- acknowledged or cleared low-severity evidence may be visually deprioritized while remaining counted as evidence;
- active generation `>= 3` is visibly marked as flapping and is not hidden by the low-priority budget;
- storm limiting exposes suppression counts instead of silently discarding evidence;
- terminal and web use the same suppression/flapping decisions.

Noise control must never acknowledge, resolve, clear or execute anything.

## Escalation timeline and aging acceptance

Aging and escalation presentation must consume only supplied control-plane evidence.

The presentation layer must not read the local wall clock and must not invent escalation deadlines.

Canonical timestamp evidence uses UTC ISO-8601 `...Z` values. Supported aging presentation states are:

- `FRESH`;
- `DUE`;
- `OVERDUE`;
- `AGING UNAVAILABLE`;
- lifecycle overrides `ACKNOWLEDGED` and `CLEARED`.

Missing or malformed timestamp evidence must fail visibly to `AGING UNAVAILABLE` rather than guessing.

Terminal and `/api/operations` must expose the same bounded escalation timeline, due/overdue counts and latest escalation actor/reason evidence where supplied.

AP-19 does not send notifications or advance escalation state.

## Authority acceptance

AP-19 must not introduce any presentation action that can:

- send notifications;
- advance escalation state;
- create acknowledgement requests;
- acknowledge or resolve alerts or incidents;
- clear a kill switch;
- resume entries;
- approve governed actions;
- mutate Risk or Execution state;
- synthesize fills;
- override exchange-confirmed execution truth.

## Cross-surface acceptance

Terminal and web must remain semantically aligned for:

- alert ids;
- severity;
- lifecycle state;
- generation;
- acknowledgement state;
- attention/suppression class;
- flapping state;
- storm/suppression counts;
- escalation aging state;
- due/overdue counts;
- read-only authority.

## Regression acceptance

Final-head Core CI must execute and pass the AP-19 regressions, including at least:

- `operator_alert_policy`;
- `operator_alert_lifecycle`;
- `operator_alert_center_view`;
- `operator_alert_center_integration`;
- `operator_alert_attention_policy`;
- `operator_alert_escalation_timeline`;
- `dashboard_operations_overlay`;
- existing terminal/operator regressions;
- ASan;
- UBSan;
- TSan.

The final head must also remain mergeable and the pull request must stay Draft until the required Core CI evidence succeeds for that same head.

## Acceptance state

Current implementation state: `IMPLEMENTED / CI PENDING`.

AP-19 status is one of:

- `IMPLEMENTED / CI PENDING` — scope is complete but final-head CI has not succeeded;
- `ACCEPTED` — final-head Core CI including required regressions and sanitizers completed successfully;
- `BLOCKED` — final-head CI or required acceptance regressions failed.

Mergeable GitHub state, documentation or an earlier green run does not substitute for final-head CI evidence.
