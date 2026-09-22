# Governed Notification Dispatch Runtime

## Purpose

Sentum executes governed notification routing intents through bounded provider adapters without granting the notification path trading, safety, incident or workflow authority.

The runtime reuses the existing routing policy, delivery state machine, provider boundary and durable notification evidence repository. Provider I/O runs only on dedicated notification workers and never on Market Data, Strategy, Risk or Execution hot paths.

## Runtime ownership

Paper and Testnet modes own the notification runtime alongside their existing long-running components. Startup occurs after the trading runtime is initialized. Shutdown stops notification intake and cancels bounded provider I/O before the trading runtime is torn down.

Dashboard-only, Replay and Research modes do not dispatch notifications.

The runtime owns a bounded scheduler loop, a fixed worker pool, one bounded immediate/retry queue, the read-write NotificationDeliveryEvidenceRepository and an immutable provider registry. Operations and dashboard consumers continue to open notification evidence read-only.

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

Recovery fails closed if the durable current-state set is truncated or exceeds the configured queue bound.

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

## Operational metrics

The existing notification operations projection exposes a dispatch_runtime object containing runtime status, queued work, active dispatch count, delivered count, retriable and terminal failure counts, provider latency, timeout count, queue rejection count and execution_authorized = false.

Durable delivery evidence remains the authority for current notification health and incident-candidate derivation. Runtime counters are diagnostic metrics and do not replace durable state.

## Shutdown

Shutdown rejects new submissions, signals provider cancellation, wakes scheduler and workers, allows active providers to exit through their bounded timeout/cancellation path, persists any resulting delivery transition, leaves queued durable work for restart recovery, joins owned threads and publishes stopped metrics.

No unbounded thread creation or unbounded provider polling is used.

## Authority isolation

Notification dispatch can never acknowledge or resolve operator alerts, create or mutate incidents, clear kill switches, resume entries, approve governed actions, mutate Risk or Execution state, synthesize fills or override exchange-confirmed execution truth.

Every dispatch request, provider result, durable evidence row and operations projection retains execution_authorized = false.

## Validation

notification_dispatch_runtime_tests covers delivery success, provider rejection, timeout, retry, maximum attempts, duplicate suppression, restart recovery, delivered-state suppression, queue saturation, unknown providers, configuration validation, cancellation, provider exceptions, authority isolation and final evidence propagation into the existing notification operations and governed incident-candidate projections.

The target is registered in CTest and attached to the existing ASan, UBSan and TSan build dependency graph.
