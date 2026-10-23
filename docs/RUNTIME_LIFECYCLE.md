# Runtime Lifecycle and Shutdown Contract

This document defines the ownership and shutdown expectations for Sentum runtime components. The contract is operational: shutdown must be bounded, wake blocking workers explicitly, preserve object lifetime until owned threads have stopped, and leave diagnostics when a deadline is exceeded.

## Global shutdown sequence

For long-running CLI modes the process signal handler only flips the atomic shutdown request. Runtime code performs the actual shutdown from the owning thread.

The required ordering is:

1. stop accepting new work;
2. stop producers and external intake;
3. wait until producers can no longer publish tail work;
4. drain or abort queued work according to the component policy;
5. wake blocked consumers;
6. join owned threads;
7. close resources only after their worker threads can no longer access them;
8. publish the final stopped state.

Destructors are a last safety net and must not be the primary shutdown mechanism for long-running modes.

## Ownership inventory

| Component | Owner | Start trigger | Stop signal / wake-up | Join location | Queue / drain policy | Expected stop bound |
| --- | --- | --- | --- | --- | --- | --- |
| `DashboardServer` | owning CLI mode (`paper`, `testnet`, `dashboard`) | `DashboardServer::start()` after bind/listen succeeds | `running_=false`, `io_context::stop()` | `DashboardServer::stop()` | pending HTTP work is aborted when the private I/O context is destroyed after join | normal target <= 2 s |
| `Collector` WebSocket worker | `ExecutionEngine` | `Collector::start()` | `running=false`, WebSocket close/stop | `Collector::stop()` | producer completion is published before the persistence writer may exit | bounded by WebSocket shutdown plus writer drain |
| `Collector` persistence writer | `Collector` | `Collector::start()` | `running=false`, producer-completion barrier, `queue_cv.notify_all()` | `Collector::stop()` | commits accepted candles or latches a visible failure with retained/unrecovered accounting | 1.5-second retry-scheduling budget / eight attempts per batch, plus in-flight transaction cleanup |
| `AsyncLogger` worker | each owning runtime component | `AsyncLogger::start()` | `running=false`, `cv.notify_all()` | `AsyncLogger::stop()` | drains queued log messages before exit | bounded by queued file writes |
| `ExecutionEngine` coordinator | `paper_main` | `ExecutionEngine::start()` | `running=false`, shutdown CV notification | `ExecutionEngine::stop()` | no independent queue | <= current coordinator wait after explicit wake |
| `ExecutionEngine` scanner worker | `ExecutionEngine` | coordinator startup | `running=false`, scanner CV notification | `ExecutionEngine::stop()` | pending symbol notification may be discarded during shutdown | bounded after explicit wake |
| paper `TradeEngine` worker | `ExecutionEngine` | symbol selection/runtime control | `TradeEngine::stop()` | `ExecutionEngine::stop_trader()` | trade engine owns its internal completion policy | bounded by trade engine stop contract |
| `NotificationDispatchRuntime` | owning `paper` / `testnet` CLI mode | after trading runtime initialization when dispatch is enabled | reject submissions, provider cancellation, scheduler/worker wake-up | `NotificationDispatchRuntime::stop()` | durable queued state is left for restart recovery; active provider I/O is bounded | configured provider timeout plus worker join |\n| `TerminalUi` | owning CLI mode | mode startup when stdout is a terminal | `running_=false` | `TerminalUi::stop()` | no durable queue | bounded by refresh/input loop interval |
| `TestnetStrategyRuntime` | `testnet_main` stack | `runtime.start()` | `running_=false`, then market stream stop and execution venue stop | component stop methods own their worker joins | no synthetic fills; exchange-confirmed state remains authoritative | bounded by market stream and venue stop contracts |
| `BinanceWebsocketClient` | trade/testnet runtime | `start()` | `running=false`, WebSocket close/stop | `stop()` | no durable queue | bounded by WebSocket shutdown |

## Dashboard lifecycle

`DashboardServer::start()` establishes readiness synchronously by parsing the bind address, opening the acceptor, binding, and listening before it starts the I/O thread. A successful return therefore means the listening socket is established.

Request accept/read/write operations are asynchronous on a private `io_context`. `stop()` never closes the acceptor while another thread is inside a synchronous `accept()`. Instead it stops the I/O context, joins the I/O thread, and only then closes and destroys the acceptor and context. Repeated start/stop creates a fresh I/O context so stale pending handlers cannot leak into the next lifecycle.

The CI dashboard smoke adds a second containment layer: every HTTP request has a client timeout, SIGTERM shutdown gets a two-second process budget, the entire smoke is capped at 60 seconds, and failure output includes process/thread state plus the dashboard log.

## Queue shutdown policy

### Paper trader ownership and replacement

The coordinator and scanner share a single trader ownership mutex. Runtime-control changes hold that mutex across the flat-position check, cancellation, worker join, final account settlement and replacement launch. Scanner requests recheck the active trader while holding the same mutex; a rejected request cannot change the current symbol. A completed worker is stopped and joined, and its final realized profit is settled, before its instance is replaced.

Flat-only cancellation is serialized with strategy decisions. If an entry completes while a strategy or symbol change is pending, replacement remains deferred until that position exits. If cancellation wins while the trader is flat, subsequent queued decisions cannot create a new position on that instance.

Each `TradeEngine` has one worker lifetime. Its stop request remains latched even when cancellation happens before `run()` begins, and another `run()` call cannot restart that instance. `stop()` signals the queue predicate under its mutex and wakes the worker. The worker owns creation and teardown of its price stream, execution venue and logger; cancellation never races a second thread against partially initialized resources. Pending price decisions are discarded after cancellation, and the owner joins the worker before reading final totals or destroying it.

A trader worker exception marks runtime health as `error`, stops the coordinator and scanner, and prevents automatic replacement. Shutdown still joins the failed worker and completes account settlement and the remaining subsystem cleanup. It then reports the retained worker exception instead of publishing successful completion. This behavior does not provide restart reconciliation for an open position; recovery and durable trade-finalization rules are separate contracts.

If account settlement also fails, the original trader exception remains the propagated failure. The final runtime diagnostic, shutdown progress and core log include both failures, preserving any account uncertainty warning and retained recovery-file path.

`trader_lifecycle_tests` exercises early cancellation before the real worker entry point, cancellation concurrent with an entry decision, idle queue wake-up, scanner contention during active ownership and replacement, requested strategy/symbol identity, partial-start failure propagation, and shutdown during replacement. Gated local workers control the relevant ordering; no exchange connection is required.

### Collector persistence

The collector rejects new external intake by clearing `running`, then stops and joins the WebSocket producer. The persistence writer has a separate producer-completion state and is not allowed to exit merely because `running` became false. It remains alive until the producer has published completion and the queue is empty. This prevents a final WebSocket callback from enqueueing a kline after the persistence writer has already exited.

The writer wake predicate follows the same contract: during shutdown it sleeps while the producer can still publish and wakes for either queued work or final producer completion. Once producer completion is visible, remaining SQLite batches are committed before the writer is joined. A batch is cleared only after a successful commit.

Retryable SQLite contention retains the batch through a bounded retry policy. A permanent error or exhausted retry budget latches failure, cancels intake and retains the current batch and remaining queue. After producer and writer joins, the owner publishes exact unrecovered accounting and propagates a failed run. It must not report that backlog as successfully drained. The terminal failure survives stop and prevents automatic reuse of the collector. See [Collector persistence](COLLECTOR_PERSISTENCE.md) for error classification, timing, counters and the limits of in-memory retention.

### Notification dispatch\n\nNotification dispatch stops accepting new work before the trading runtime is stopped. Provider cancellation is signalled and all notification workers are explicitly awakened. Active HTTP calls are bounded by configured connect/request timeouts and cancellation. Queued work is not forced through provider I/O during shutdown; its already-persisted `PENDING` or retriable `FAILED` state is intentionally left for restart recovery.\n\n### Async logger

The logger queue is always inspected and swapped while holding its mutex. On stop the worker is explicitly awakened and exits only when the queue is empty. Concurrent producers cannot race an unlocked `std::queue::empty()` check.

## Concrete WebSocket ownership

`BinanceWebsocketClient` and `BinanceUserDataStream` initialize a fresh Asio endpoint synchronously before publishing each worker. Calling `stop()` or destroying either client before `start()` is safe. A stop request remains effective while worker preparation is pending, so delayed preparation cannot create a connection after cancellation.

Start and stop serialize thread ownership. The owner joins a previous worker before replacing its endpoint; repeated and competing stop calls converge on the same joined state. Joining releases the lifecycle mutex so a message callback can call `stop()` while another thread is waiting for that callback to finish. Callback-driven stop requests cancellation; the owning thread subsequently joins and destroys the client. Destroying the client from its own callback is outside this ownership contract.

A failed startup can be stopped safely and followed by an explicit restart using a new endpoint. Worker liveness is separate from authenticated subscription readiness and connection recovery; this lifecycle contract does not establish either of those properties.

`websocket_lifecycle_tests` exercises both concrete clients with controlled preparation and callback gates, including failed startup, early cancellation, immediate destruction, callback-stop during an owner join, and repeated/concurrent start-stop. These tests use real local Asio initialization and cancellation without contacting an exchange.

## CI lifecycle evidence

The standard CI release job must run the registered CTest lifecycle suite before the CLI, research, dashboard, and benchmark smoke checks. The sanitizer matrix executes the same lifecycle suite under AddressSanitizer, UndefinedBehaviorSanitizer, and ThreadSanitizer.

Hard limits:

- runtime lifecycle CTest: 60 seconds;
- dashboard smoke: 60 seconds overall;
- dashboard SIGTERM-to-exit budget inside the smoke: 2 seconds;
- each CI job: 10 minutes.

A timeout is a failure, not a retry or an implicit pass. Sanitizer suppressions are not part of this contract; an external false positive requires separate documented evidence before any suppression is introduced.

## Regression scenarios

`runtime_lifecycle_tests` covers:

- concurrent logger producers followed by a draining stop;
- collector shutdown with a non-empty persistence queue and verification that the complete backlog reaches SQLite;
- collector shutdown while hermetic mock market traffic is still publishing, including producer-tail drain and enqueue/persistence accounting;
- 20 repeated dashboard start/stop cycles;
- dashboard cycles both with and without HTTP traffic;
- the two-second dashboard stop budget;
- start failure on an occupied port;
- safe stop after partial initialization failure;
- recovery and successful restart after the failed initialization.

Network-dependent exchange connectivity remains covered by dedicated integration paths. The hermetic lifecycle executable deliberately exercises ownership, producer completion, queue drain, and shutdown timing without depending on Binance availability.
