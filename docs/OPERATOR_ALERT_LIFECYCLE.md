# Operator Alert Episodes, Deduplication and Acknowledgement

## Ownership

The notification scheduler owns durable episode identity for the code-owned alerts
from `OperatorAlertPolicy.hpp`. Before it creates notification routes, it observes the
current conditions and commits their lifecycle through
`NotificationDeliveryEvidenceRepository::observe_alert_lifecycle()` in the configured
runtime SQLite database. Delivery and presentation consume that committed identity.

The lifecycle does not grant incident, acknowledgement or trading authority.
`execution_authorized` remains `false` in stored rows, delivery requests and views.

## Lifecycle states

| State | Meaning |
| --- | --- |
| `NEW` | First observation of the current episode. |
| `ACTIVE` | The same active episode was observed again. |
| `ACKNOWLEDGED` | Matching acknowledgement evidence was observed for this episode. |
| `CLEARED` | The condition was absent in a committed observation. |

A stable alert id has one latest lifecycle row. The first observed activation starts
at generation 1. Repeated active observations retain that generation. After a
committed clear, the next activation increments it and starts a new episode. Updated
severity, description and escalation remain part of the current condition; they do
not independently create a new episode.

Cleared rows are retained as tombstones. One or many healthy observations cannot
remove the generation high-water mark. The bounded lifecycle contains stable alert
identities rather than an unbounded row for every poll or past episode. Delivery
history remains separately append-only.

## Restart and migration

Restart does not itself create an alert episode. A persisted active generation plus
the same active condition remains that generation and retains its acknowledgement and
delivery deduplication. A persisted clear followed by an active condition starts the
next generation. If a process stops after committing the new episode but before
appending its delivery intent, the next process routes the same committed generation.

Older databases have delivery history without lifecycle rows. Reconciliation seeds
missing identities from the maximum recorded delivery generation for each alert,
including delivered and terminal-failed notifications. It treats that prior generation
as active until an observation proves otherwise. This prevents generation 1 from
colliding with an already-used legacy delivery key or a higher recorded generation.

Neither delivery completion nor a restart proves that a condition cleared. An outage
that clears and recurs entirely while no scheduler is observing cannot be distinguished
from one continuous outage. The runtime conservatively retains the last active
generation until it commits a healthy observation. It does not fabricate events during
downtime. The same rule applies if persistence of a clear fails.

## Transaction and failure boundary

A SQLite `BEGIN IMMEDIATE` transaction reads prior identities and legacy generation
floors, reconciles the current conditions, updates lifecycle rows and commits an
observation checkpoint. Routing starts only after that commit. A failed lifecycle
write rolls back the observation and prevents new notification intents from that
snapshot. The scheduler reports degraded status and unavailable lifecycle evidence;
subsequent polls may retry the observation. Previously accepted delivery work retains
its own durable identity and recovery contract.

Generation and observation values are positive integers within the supported SQLite
integer range. Corrupt, rounded or contradictory metadata is rejected. A generation
at the maximum supported integer cannot wrap to a previously used key: reactivation
fails closed. Queries retain their configured bounds; incomplete state is never used
to allocate a generation.

## Acknowledgement evidence

Acknowledgements are read from:

`operations_control_plane.alert_acknowledgements[]`

An item identifies `alert_id`, positive integer `generation`, and optional `actor` and
`reason`. The scheduler matches both id and generation. An observed acknowledgement
persists for that active episode across restarts. Clearing and reactivation do not
carry it into the new generation. Invalid or generation-zero evidence is ignored.

The UI cannot create acknowledgements. A matching acknowledgement suppresses new
routing for its generation; it does not authorize execution or rewrite already
accepted delivery history.

## Current conditions and read-only views

The runtime publishes its committed lifecycle and availability in
`notification_dispatch_runtime.alert_lifecycle` and `alert_lifecycle_status`.
Read-only operations views load the same SQLite lifecycle and replace cached episode
evidence before rendering. The scheduler always derives conditions from the runtime
fields themselves, so a projected lifecycle cannot feed back as a new condition or
advance its own generation.

Current conditions remain visible even when episode persistence is unavailable or
has not yet observed them. Such rows render as `UNTRACKED` with generation 0, no
inherited acknowledgement and no notification candidacy. A stale stored active row
cannot override a current clear, and a new raw kill switch or outage cannot disappear
because the last stored lifecycle was empty. Presentation never allocates a durable
generation. Missing or uninitialized lifecycle storage clears cached identity authority.

## Authority boundary

Alert lifecycle persistence and presentation cannot acknowledge or resolve incidents,
clear a kill switch, resume entries, approve governed actions, alter risk/execution
state, synthesize fills or override exchange-confirmed truth. Notification delivery
remains subject to the governed routing policy and provider boundary.

## Regression coverage

`operator_alert_lifecycle_tests` retains the pure reconciliation and acknowledgement
checks. `notification_alert_generation_tests` exercises the running scheduler and an
in-process recording provider with real SQLite persistence:

- disconnect, repeated healthy observations and a second disconnect;
- recurring kill switches and recurrence after terminal provider rejection;
- restarts during an active episode, after clear and after committed recurrence;
- migration from multiple legacy generations without resetting delivery deduplication;
- acknowledgement retention within an episode and isolation from later episodes;
- failed initial lifecycle writes and failed clears, with rollback and no new delivery;
- current-condition visibility under uninitialized, stale and corrupt episode storage;
- integer exhaustion and rounded JSON identity rejection.

The suite is registered in CTest and the existing ASan, UBSan and TSan dependency graph.
No external notification or exchange request is required.
