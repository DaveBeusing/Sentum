# Notification Incident Workflow

AP-22 introduces a governed bridge from AP-21 notification delivery health into the existing incident workflow surface.

## Purpose

Notification delivery failures may require an operator incident, but notification infrastructure is not allowed to create or mutate production incident state on its own. The bridge therefore produces a proposal only.

## Candidate states

- `NONE`: delivery health does not require incident escalation.
- `ATTENTION`: delivery requires operator attention but does not meet the incident proposal threshold.
- `PROPOSAL_READY`: terminal delivery failure evidence supports a governed incident proposal.
- `BLOCKED`: required delivery evidence is unavailable; the bridge fails closed.

## Governed proposal

A proposal-ready candidate exposes:

- action: `OPEN_INCIDENT`;
- classification: `APPROVAL_REQUIRED`;
- source: `NOTIFICATION_OPERATIONS`;
- terminal-failure and backlog evidence;
- `incident_authorized = false`;
- `execution_authorized = false`.

The proposal is input for the governed incident lifecycle. It is not an incident mutation and it does not bypass approval policy.

A stable source-correlation identity is derived from terminal notification evidence. The lifecycle runtime uses that identity to submit an idempotent `OPEN_INCIDENT` request. Re-observing the same terminal failure therefore cannot create a second request.

## Authority boundary

The bridge must not:

- create, acknowledge, resolve or close an incident;
- acknowledge or clear an operator alert;
- clear a kill switch;
- resume entries;
- approve a governed action;
- mutate Risk or Execution state;
- synthesize fills;
- override exchange-confirmed execution truth.

Missing notification evidence fails closed and exposes no actionable incident proposal.

## Durable evidence integration

The incident candidate is derived from the same durable latest-state notification evidence used by operations observability. Delivery evidence is loaded read-only from the configured runtime SQLite database and remains ordered by persistent sequence.

If durable evidence is missing, unreadable, corrupt or incomplete because a bounded current-state query was truncated, the notification health projection becomes `UNAVAILABLE` and the incident candidate becomes `BLOCKED`.

Incident request, approval, audit and recovery evidence now comes from the durable governed incident lifecycle in the operations control plane. Notification persistence itself still cannot approve or advance an incident: the runtime may create only the approval-required request, and every later state change is an explicit operator-controlled lifecycle transition.

Regression coverage verifies that terminal durable delivery failures produce the same governed proposal semantics as equivalent in-memory evidence and that approval, audit and recovery correlation remains unchanged across repository reopen.
