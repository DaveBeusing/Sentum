# Notification Incident Workflow Acceptance

The notification incident workflow is accepted only when the exact final pull-request head satisfies this contract and Core CI succeeds for that head.

## Required capabilities

- notification operations health can be translated into a governed notification-incident candidate.
- Candidate states are `NONE`, `ATTENTION`, `PROPOSAL_READY`, and `BLOCKED`.
- Only an `INCIDENT_CANDIDATE` backed by terminal notification-delivery failure evidence can produce `PROPOSAL_READY`.
- A proposal exposes only `OPEN_INCIDENT` with `APPROVAL_REQUIRED` classification.
- Missing notification evidence fails closed as `BLOCKED` and exposes no incident-opening action.
- `ATTENTION` remains advisory and does not create an incident proposal.
- Proposal state never creates, acknowledges, resolves, or mutates an incident.
- Existing control-plane incident, approval, audit, and recovery evidence is correlated by `OPEN_INCIDENT` and request identity; evidence is never invented locally.
- Existing recovery workflow state is presentation context only. The notification incident workflow does not create or advance recovery state.
- `/api/operations` exposes the governed notification incident workflow together with request, approval, audit, incident, and recovery evidence.
- The web Operations surface renders the same read-only notification incident workflow contract.
- The web surface continues to expose no POST, PUT, PATCH, or DELETE operations routes.

## Authority boundaries

The notification incident proposal and integration layers must keep:

- `incident_authorized = false`;
- `execution_authorized = false`.

The notification incident workflow must not:

- create, acknowledge, resolve, or close incidents;
- acknowledge or resolve alerts;
- approve its own proposal;
- clear a kill switch;
- resume entries;
- mutate Risk or Execution state;
- synthesize fills;
- override exchange-confirmed execution truth.

Actual incident and recovery state remain upstream control-plane truth.

## Required regression evidence

The final head must run the notification incident bridge regression together with existing cross-surface and dashboard regressions. The notification incident test target remains attached to the sanitizer build graph.

Required scenarios include:

1. healthy notification operations -> no proposal;
2. attention -> advisory only;
3. terminal delivery failure -> `PROPOSAL_READY / OPEN_INCIDENT / APPROVAL_REQUIRED`;
4. incident-candidate health without terminal failure -> no proposal;
5. unavailable evidence -> fail-closed `BLOCKED`;
6. proposal without control-plane evidence -> no invented request, approval, audit, incident, or recovery truth;
7. existing `OPEN_INCIDENT` approval/audit evidence -> correlated by request identity;
8. unrelated approval/audit actions -> not attached;
9. existing recovery workflow -> exposed as read-only context;
10. `/api/operations` and dashboard expose the same read-only workflow;
11. all presentation paths retain `incident_authorized = false` and `execution_authorized = false`.

## Final-head rule

Earlier successful workflow runs are supporting evidence only. The notification incident workflow is accepted only after Core CI succeeds for the exact final head containing the bridge, evidence integration, cross-surface projection, dashboard presentation, regressions, documentation, and this acceptance contract. Until then the PR remains Draft and status is `IMPLEMENTED / CI PENDING`.
