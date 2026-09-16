# Operator Alert Lifecycle, Deduplication & Acknowledgement UX

## Purpose

This AP-19 slice adds a deterministic presentation lifecycle around the code-owned operator alerts introduced by `OperatorAlertPolicy.hpp`.

The lifecycle is presentation-only. It does not create a mutable alert service, notification provider, acknowledgement endpoint or incident authority.

## Lifecycle states

Each stable alert id is projected as one of:

- `NEW` — first observation of the current alert generation;
- `ACTIVE` — the same active alert was observed again;
- `ACKNOWLEDGED` — matching control-plane acknowledgement evidence exists for the current generation;
- `CLEARED` — the alert condition disappeared from the current runtime snapshot.

`execution_authorized` remains `false` in every state.

## Stable ids and deduplication

Alerts are deduplicated by the code-owned stable alert id emitted by `OperatorAlertPolicy.hpp`.

Repeated observations of the same id do not create duplicate instances. The latest presentation text is retained and the observation count advances.

This prevents repeated snapshots of one condition from creating an alert storm in the presentation layer.

## Generations and re-activation

Each alert lifecycle item has a generation number.

The first activation starts at generation `1`. When an alert clears and later becomes active again, the next instance is `NEW` with generation incremented by one.

This matters for acknowledgement safety: acknowledgement evidence is matched against both `alert_id` and `generation`. An acknowledgement from a previous incident cannot silently acknowledge a later re-activation of the same stable alert id.

## Acknowledgement evidence

Acknowledgements are read from optional control-plane snapshot evidence:

`operations_control_plane.alert_acknowledgements[]`

Each accepted evidence item contains:

- `alert_id`;
- `generation`;
- optional `actor`;
- optional `reason`.

Malformed entries, missing ids and generation `0` are ignored.

The UI does not create or mutate acknowledgement evidence. Acknowledgement becomes visible only when upstream control-plane truth contains a matching `(alert_id, generation)` record.

## Clear behavior

When an alert disappears from the current derived alert set, the prior active item is retained for one lifecycle projection as `CLEARED` and marked inactive.

Already-cleared items are not repeatedly duplicated on subsequent reconciliations.

A later re-appearance starts a new generation.

## Authority boundary

The lifecycle layer cannot:

- acknowledge or resolve an alert;
- clear a kill switch;
- resume entries;
- approve a governed action;
- mutate incident/recovery state;
- mutate Risk or Execution truth;
- synthesize fills;
- override exchange-confirmed execution truth.

Acknowledgement is presentation evidence only and never grants execution authority.

## Regression coverage

`operator_alert_lifecycle_tests` verifies:

- `NEW -> ACTIVE -> CLEARED` transitions;
- stable-id deduplication;
- acknowledgement requires an exact generation match;
- re-activation increments generation;
- old acknowledgement evidence does not leak into a new generation;
- control-plane acknowledgement evidence is projected read-only;
- acknowledgement never sets execution authority.

The test target is registered in CTest and attached to the existing sanitizer dependency graph for ASan, UBSan and TSan.
