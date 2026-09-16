# Operator Alert Center

## Purpose

AP-19 integrates the alert classification and lifecycle presentation into both operator surfaces without creating a new alert authority or notification channel.

Terminal and web consume the same `OperatorAlertCenterView` projection.

## Shared alert center

The alert center exposes:

- total alert count;
- active count;
- acknowledged count;
- cleared count;
- critical, warning and attention counts;
- bounded alert rows with id, source, severity, lifecycle state, generation, title, workspace guidance and acknowledgement evidence.

Every alert row keeps `execution_authorized = false`.

## Lifecycle evidence

The presentation layer consumes `operations_control_plane.alert_lifecycle[]` when authoritative lifecycle evidence is present in the runtime snapshot.

Supported lifecycle states are:

- `NEW`;
- `ACTIVE`;
- `ACKNOWLEDGED`;
- `CLEARED`.

A lifecycle row may include generation, actor and acknowledgement reason.

The renderer does not create or mutate lifecycle history. In the absence of persisted lifecycle evidence, currently derived alerts are presented as `ACTIVE`; generation-matching acknowledgement evidence may present them as `ACKNOWLEDGED`. The presentation layer does not invent cleared history.

## Terminal integration

The terminal operator surface renders a bounded Alert Center summary and up to six alert rows before the existing workflow, approval and audit sections.

Alert-center rendering remains inside the existing terminal frame/diff path. No second output path is introduced.

## Web integration

`GET /api/operations` now contains an `alerts` section with the same shared alert-center counts and rows.

The read-only Operations dashboard renders:

- active alert count;
- acknowledged alert count;
- cleared alert count;
- critical / warning count;
- lifecycle rows in the Alert Center panel.

The browser performs no alert reclassification and exposes no acknowledgement or resolve write action.

## Cross-surface consistency

`operator_alert_center_integration_tests` verifies that the terminal and serialized browser contract use the same alert ids, severities, lifecycle states and counts.

`operator_alert_center_view_tests` verifies current alerts, acknowledgement evidence, persisted cleared history and bounded rendering.

Both regression targets are attached to the sanitizer build graph.

## Authority boundary

The Alert Center cannot:

- acknowledge an alert;
- resolve or clear an alert;
- acknowledge or resolve an incident;
- clear a kill switch;
- resume entries;
- approve governed actions;
- send an external notification;
- mutate Risk or Execution;
- synthesize fills;
- override exchange-confirmed execution truth.

AP-19 notification delivery and routing remain separate later slices.
