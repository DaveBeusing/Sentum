# Governed Notification Dispatch Runtime

## Purpose

Sentum executes governed notification routing intents through bounded provider adapters without granting the notification path trading, safety, incident or workflow authority.

The runtime reuses the existing routing policy, delivery state machine, provider boundary and durable notification evidence repository. Provider I/O runs only on dedicated notification workers and never on Market Data, Strategy, Risk or Execution hot paths.

## Runtime ownership

Paper and Testnet modes own the notification runtime alongside their existing long-running components. Startup occurs after the trading runtime is initialized. Shutdown stops notification intake and cancels bounded provider I/O before the trading runtime is torn down.

Dashboard-only, Replay and Research modes do not dispatch notifications.

The runtime owns a bounded scheduler loop, a fixed worker pool, one bounded immediate/retry queue, bounded startup recovery staging, the read-write NotificationDeliveryEvidenceRepository and an immutable provider registry. Operations and dashboard consumers continue to open notification evidence read-only.

## Delivery flow

For each eligible governed routing intent the runtime:

1. suppresses generation-safe duplicates using durable current-state keys;
2. persists PENDING;
3. queues the delivery outside trading paths;
4. persists DISPATCHED;
5. creates NotificationDispatchRequest through the existing provider boundary;
6. executes the configured provider with bounded connect/request timeouts and cancellation;
7. applies NotificationDispatchResult through the existing state machine;
8. persists DELIVERED or FAILED;
9. schedules a retry only when the existing state machine marks the failure retryable.

Retry timing is taken from retry_backoff_seconds. The dispatcher does not implement a second backoff algorithm. A provider result marked non-retryable becomes terminal immediately. Retryable failures become terminal when the state-machine attempt budget is exhausted.

## Restart recovery and idempotency

At startup the runtime loads the bounded durable latest state for every dedup key.

- DELIVERED and terminal FAILED keys remain closed.
- PENDING work is requeued.
- retryable FAILED work is requeued after its persisted backoff.
- an interrupted DISPATCHED state is first persisted as retryable FAILED / INTERRUPTED and then rescheduled using state-machine backoff.

The HTTP provider sends the generation-safe delivery key in the Idempotency-Key header so compatible providers can suppress duplicate transport effects when a process stops after provider acceptance but before durable confirmation.

Recovery fails closed if the durable current-state set exceeds `recovery_limit` or the repository query bound. Active durable work can exceed `queue_capacity`: a previous process could have both queued and in-flight deliveries. Recovery stages those rows within `recovery_limit` and replenishes the immediate/retry queue as workers take items. The queue never exceeds `queue_capacity`; staged recovery work is reported separately as `recovery_pending`. Restored attempts retain their delivery key, attempt budget and persisted backoff, and every provider call still requires a committed DISPATCHED transition.

## Bounded execution

config/notification_dispatch.json controls runtime bounds:

- queue capacity: 1..4096;
- workers: 1..8;
- maximum attempts: 1..10;
- snapshot poll interval: 100..60000 ms;
- recovery keys: 1..4096;
- provider connect timeout: 100..30000 ms;
- provider request timeout: 100..30000 ms.

The connect timeout must not exceed the request timeout.

Queue saturation never silently discards eligible work. A rejected new delivery is persisted as terminal FAILED / QUEUE_SATURATED. A rejected retry is persisted as terminal FAILED / RETRY_QUEUE_SATURATED.

## Evidence write failures

Failures writing PENDING, DISPATCHED, a provider result or a terminal queue rejection latch `DEGRADED`, set `recovery_required`, record the first error and pause new notification admission. Every worker has an exception boundary, including unknown exceptions. A notification storage error cannot escape its worker and terminate the trading process.

Already accepted queued work may finish if its required evidence writes succeed. A failed DISPATCHED write never calls the provider. When a later write fails, the preceding durable PENDING, DISPATCHED or retryable FAILED record remains the recovery authority. In particular, failure to persist RETRY_QUEUE_SATURATED leaves the prior retryable failure intact. The runtime does not claim that an uncommitted terminal transition or provider result succeeded.

`queue_rejections` counts actual capacity rejections whether or not their terminal record commits. `terminal_failed` and `delivered` count only successfully persisted results. `persistence_errors` identifies failed evidence writes; `worker_errors` counts exceptions contained by the worker boundary. These counters are separate from scheduler `snapshot_errors`.

After the storage fault is repaired, stopping and restarting the notification runtime reloads durable state before clearing the admission latch. Stopping alone preserves `DEGRADED`, `last_error` and `recovery_required`; `running=false` distinguishes the stopped runtime. Scheduler errors cannot replace this first delivery failure or reopen admission. Shutdown clears only in-memory queues and recovery staging; their durable records remain available for restart.

Lifecycle transitions are serialized. A concurrent restart waits for all previous workers and any in-flight admission write before reconstructing durable delivery state; a late provider result cannot overwrite the restarted worker ownership or clear its failure accounting. Cancellation requested from a provider, snapshot or metrics callback only signals shutdown. An external `stop()` or subsequent `start()` owns the actual joins, so callbacks never join themselves or wait for an owner already joining them. `running` remains true while those owned threads still require a join.

If a provider accepted a notification but the result could not be persisted, recovery may call that provider again with the same delivery key. Suppressing a second external effect requires the provider's idempotency support. No exactly-once external delivery guarantee is claimed for providers without that support.

## Provider registry

Providers are selected explicitly by governed notification channel. An unknown or unconfigured channel fails closed as terminal PROVIDER_UNAVAILABLE.

The production adapter currently supported by the runtime is HTTP_JSON, implemented with the repository's existing libcurl dependency. It requires an HTTPS endpoint.

HTTP result handling is:

- 2xx: delivered;
- 408, 425, 429 and 5xx: retryable failure;
- other HTTP errors: terminal failure;
- request timeout: retryable TIMEOUT;
- cancellation during shutdown: retryable CANCELLED;
- transport/library errors: retryable and bounded by the state-machine attempt budget.

Deterministic tests use in-process provider implementations and never make external network calls.

## Configuration

The repository example is safe by default and sets enabled to false, queue_capacity to 128, worker_count to 2, max_attempts to 3, snapshot_poll_interval_ms to 500 and recovery_limit to 1024 with no enabled channels or providers.

A production provider entry contains channel, type HTTP_JSON, an HTTPS endpoint, optional credential_environment, connect_timeout_ms and request_timeout_ms.

credential_environment is the name of an environment variable, not a secret. The secret value must never be committed. If a configured credential environment variable is unavailable, the provider fails closed with terminal CREDENTIAL_UNAVAILABLE.

If notification_dispatch.json is absent, notification dispatch remains disabled and the runtime metrics expose the reason.

The enabled runtime validates the complete routing policy before starting its scheduler or provider workers. The policy requires integer schema version 1, `NOTIFICATION_ROUTING_ONLY` authority and a `BLOCK` default. Every route supplies a supported severity (`INFO`, `ATTENTION`, `WARNING` or `CRITICAL`), an integer minimum escalation level in 0..3, and nonempty channel and audience arrays. Floating-point, boolean, negative or overflowing escalation values are rejected without numeric coercion.

Channel and audience identifiers must be nonempty and cannot contain ASCII whitespace, control characters or `:`, the delivery-key delimiter. Lists cannot repeat identifiers, and each route channel must appear in `allowed_channels`. Audiences remain policy-defined and channel identifiers remain extensible. An empty route list is a valid block-all policy. Routes retain their configured first-match order. A malformed field anywhere in the policy blocks the complete policy; it cannot leave a partially enabled route or terminate the process through a `noexcept` lookup.

## Operational metrics

The existing notification operations projection exposes a dispatch_runtime object containing runtime status, running/admission state, queued work, staged recovery work, active dispatch count, delivered count, retriable and terminal failure counts, provider latency, timeout count, queue rejection count, persistence/worker errors, recovery requirements and execution_authorized = false.

Durable delivery evidence remains the authority for current notification health and incident-candidate derivation. Runtime counters are diagnostic metrics and do not replace durable state.

## Shutdown

Shutdown rejects new submissions, signals provider cancellation, wakes scheduler and workers, allows active providers to exit through their bounded timeout/cancellation path, persists any resulting delivery transition, leaves queued durable work for restart recovery, joins owned threads and publishes stopped metrics. Unresolved evidence failures remain visible as described above. Stop changes the condition-variable predicates under the corresponding wait mutexes so an idle worker cannot miss cancellation.

No unbounded thread creation or unbounded provider polling is used.

## Authority isolation

Notification dispatch can never acknowledge or resolve operator alerts, create or mutate incidents, clear kill switches, resume entries, approve governed actions, mutate Risk or Execution state, synthesize fills or override exchange-confirmed execution truth.

Every dispatch request, provider result, durable evidence row and operations projection retains execution_authorized = false.

## Validation

notification_dispatch_runtime_tests covers delivery success, provider rejection, timeout, retry, maximum attempts, duplicate suppression, restart recovery, delivered-state suppression, queue saturation, unknown providers, configuration validation, cancellation, provider exceptions, authority isolation and final evidence propagation into the existing notification operations and governed incident-candidate projections.

`notification_routing_policy_tests` covers nested route types, missing fields, numeric boundaries, channel/audience identity validation, direct lookup, controlled runtime construction failure before provider work, the shipped policy and a valid scheduler delivery. It is also part of the sanitizer build graph.

notification_persistence_failure_tests combines a full retry queue with SQLite trigger failures, verifies continued worker/process execution and accurate durable counters, and restarts the unchanged single-slot configuration with multiple outstanding durable deliveries. It also covers initial-write rejection, provider-result uncertainty with stable idempotency keys, unknown provider exceptions, paused admission, retained failure diagnostics and provider calls only after DISPATCHED commits. All provider behavior is local to the test process.

The persistence suite also blocks an old provider during a concurrent restart, then fails its final durable write and verifies recovery of that exact attempt. A SQLite write lock verifies recovery waits for an admission still committing from the preceding run. Provider, scheduler and metrics callbacks exercise cancellation, including cancellation while an external caller is already joining the worker.

All three targets are registered in CTest and attached to the existing ASan, UBSan and TSan build dependency graph.
