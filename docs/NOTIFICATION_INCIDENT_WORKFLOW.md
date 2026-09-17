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

## Initial acceptance

The first AP-22 slice requires deterministic regression coverage for healthy, attention, incident-candidate and unavailable delivery states, plus sanitizer build integration. Cross-surface proposal presentation, approval evidence, recovery workflow integration and final AP-22 acceptance remain subsequent slices.
