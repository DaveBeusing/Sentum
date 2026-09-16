# Runtime Lifecycle and Shutdown Contract

This document defines the ownership and shutdown expectations for Sentum runtime components. The contract is operational: shutdown must be bounded, wake blocking workers explicitly, preserve object lifetime until owned threads have stopped, and leave diagnostics when a deadline is exceeded.

## Global shutdown sequence

For long-running CLI modes the process signal handler only flips the atomic shutdown request. Runtime code performs the actual shutdown from the owning thread.

The required ordering is:

1. stop accepting new work;
2. stop producers and external intake;
3. drain or abort queued work according to the component policy;
4. wake blocked consumers;
5. join owned threads;
6. close resources only after their worker threads can no longer access them;
7. publish the final stopped state.

Destructors are a last safety net and must not be the primary shutdown mechanism for long-running modes.

## Ownership inventory

| Component | Owner | Start trigger | Stop signal / wake-up | Join location | Queue / drain policy | Expected stop bound |
| --- | --- | --- | --- | --- | --- | --- |
| `DashboardServer` | owning CLI mode (`paper`, `testnet`, `dashboard`) | `DashboardServer::start()` after bind/listen succeeds | `running_=false`, `io_context::stop()` | `DashboardServer::stop()` | pending HTTP work is aborted when the private I/O context is destroyed after join | normal target <= 2 s |
| `Collector` WebSocket worker | `ExecutionEngine` | `Collector::start()` | `running=false`, WebSocket close/stop | `Collector::stop()` | producer is stopped before writer join | bounded by WebSocket shutdown plus writer drain |
| `Collector` persistence writer | `Collector` | `Collector::start()` | `running=false`, `queue_cv.notify_all()` | `Collector::stop()` | drains the remaining kline queue before exit | bounded by remaining SQLite batches |
| `AsyncLogger` worker | each owning runtime component | `AsyncLogger::start()` | `running=false`, `cv.notify_all()` | `AsyncLogger::stop()` | drains queued log messages before exit | bounded by queued file writes |
| `ExecutionEngine` coordinator | `paper_main` | `ExecutionEngine::start()` | `running=false`, shutdown CV notification | `ExecutionEngine::stop()` | no independent queue | <= current coordinator wait after explicit wake |
| `ExecutionEngine` scanner worker | `ExecutionEngine` | coordinator startup | `running=false`, scanner CV notification | `ExecutionEngine::stop()` | pending symbol notification may be discarded during shutdown | bounded after explicit wake |
| paper `TradeEngine` worker | `ExecutionEngine` | symbol selection/runtime control | `TradeEngine::stop()` | `ExecutionEngine::stop_trader()` | trade engine owns its internal completion policy | bounded by trade engine stop contract |
| `TerminalUi` | owning CLI mode | mode startup when stdout is a terminal | `running_=false` | `TerminalUi::stop()` | no durable queue | bounded by refresh/input loop interval |
| `TestnetStrategyRuntime` | `testnet_main` stack | `runtime.start()` | `running_=false`, then market stream stop and execution venue stop | component stop methods own their worker joins | no synthetic fills; exchange-confirmed state remains authoritative | bounded by market stream and venue stop contracts |
| `BinanceWebsocketClient` | trade/testnet runtime | `start()` | `running=false`, WebSocket close/stop | `stop()` | no durable queue | bounded by WebSocket shutdown |

## Dashboard lifecycle

`DashboardServer::start()` establishes readiness synchronously by parsing the bind address, opening the acceptor, binding, and listening before it starts the I/O thread. A successful return therefore means the listening socket is established.

Request accept/read/write operations are asynchronous on a private `io_context`. `stop()` never closes the acceptor while another thread is inside a synchronous `accept()`. Instead it stops the I/O context, joins the I/O thread, and only then closes and destroys the acceptor and context. Repeated start/stop creates a fresh I/O context so stale pending handlers cannot leak into the next lifecycle.

The CI dashboard smoke adds a second containment layer: every HTTP request has a client timeout, SIGTERM shutdown gets a two-second process budget, the entire smoke is capped at 60 seconds, and failure output includes process/thread state plus the dashboard log.

## Queue shutdown policy

### Collector persistence

The collector rejects new production by clearing `running`, wakes the persistence writer, and leaves the writer loop active while either the collector is running or the queue is non-empty. Existing queue contents are therefore drained before the writer thread is joined.

### Async logger

The logger queue is always inspected and swapped while holding its mutex. On stop the worker is explicitly awakened and exits only when the queue is empty. Concurrent producers cannot race an unlocked `std::queue::empty()` check.

## CI lifecycle evidence

The standard CI release job must run the registered CTest lifecycle suite before the CLI, research, dashboard, and benchmark smoke checks. The sanitizer matrix executes the same lifecycle suite under AddressSanitizer, UndefinedBehaviorSanitizer, and ThreadSanitizer.

Hard limits:

- runtime lifecycle CTest: 60 seconds;
- dashboard smoke: 60 seconds overall;
- dashboard SIGTERM-to-exit budget inside the smoke: 2 seconds;
- each CI job: 10 minutes.

A timeout is a failure, not a retry or an implicit pass. Sanitizer suppressions are not part of this contract; an external false positive requires separate documented evidence before any suppression is introduced.

## Regression scenarios

`runtime_lifecycle_tests` currently covers:

- concurrent logger producers followed by a draining stop;
- 20 repeated dashboard start/stop cycles;
- dashboard cycles both with and without HTTP traffic;
- the two-second dashboard stop budget;
- start failure on an occupied port;
- safe stop after partial initialization failure;
- recovery and successful restart after the failed initialization.

Network-dependent exchange runtimes remain covered by their dedicated integration paths rather than by this hermetic lifecycle executable. Any future mockable collector or execution-venue seam should add active-traffic shutdown and non-empty persistence-queue cases to the hermetic suite.
