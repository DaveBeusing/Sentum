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

The proposal is input for the existing governance/approval plane. It is not an incident mutation and it does not bypass approval policy.

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

Existing incident, approval, audit and recovery evidence continues to come from the operations control plane. Durable notification persistence does not create or advance any of those workflows.

Regression coverage verifies that terminal durable delivery failures produce the same governed proposal semantics as equivalent in-memory evidence and that approval, audit and recovery correlation remains unchanged across repository reopen.
