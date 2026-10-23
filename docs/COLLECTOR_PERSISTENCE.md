# Collector persistence and failure accounting

The collector accepts closed candles into an 8,192-item queue. One database writer owns
the queue consumer and a batch of at most 256 items. An accepted candle is counted as
persisted only after the complete SQLite transaction commits. Removing a candle from
the queue is not persistence evidence.

## Transaction and retry contract

Each batch uses `BEGIN IMMEDIATE`, the existing `(symbol, timestamp)` UPSERT, and
`COMMIT`. Every failed statement or commit ends the prepared statement and rolls back
an active transaction before another batch attempt. The result retains the original
SQLite error and the rollback result. Replaying a rolled-back batch or a duplicate
candle updates the same key; it does not create a second history row.

Only `SQLITE_BUSY` and `SQLITE_LOCKED`, including their extended result codes, are
retryable after a successful rollback. Constraint, I/O, full-disk, corruption and other
write failures stop collection immediately. A failed rollback also stops collection
because transaction state is no longer safe to reuse.

The current batch remains in memory for every retry. A batch gets at most eight
attempts with a 1.5-second retry-scheduling budget. Each SQLite operation uses at most
a 100 ms busy timeout; retry delays grow from 25 ms to a 200 ms maximum. An in-flight
transaction and its rollback can finish after the scheduling deadline. Shutdown wakes
the backoff wait and consumes only the remaining attempts. These bounds govern lock
contention and application waits; they cannot bound a stalled operating system or
failed physical storage device.

`Database` retains one serialized connection owner. The collector's shorter busy
timeout is restored to the existing five-second default after each call. No other
thread may use that same `Database` connection during a writer operation.

## Terminal failure

A permanent error or exhausted contention budget latches `FAILED`, rejects subsequent
persistence intake, requests market-transport cancellation and exits the writer.
The current batch and the remaining queue are retained for the collector's remaining
lifetime. They stay bounded by 256 and 8,192 items respectively. Producer work already
admitted during the failure transition remains part of the accepted total.

The owner stops and joins the producer before taking final accounting. It publishes
the retained failure after cleanup through `rethrow_persistence_failure()`. A failed
collector cannot be restarted automatically. Neither a zero queue depth nor a joined
writer can turn that failure into successful shutdown evidence.

The runtime coordinator observes the terminal failure, stops the run and joins the
trader, market producer, database writer and scanner before reporting failure to the
caller. Shutdown also checks storage after joining: a write that first fails during
the drain cannot be reported as a successful stop. Existing trader and paper-account
failure diagnostics retain their priority; the collector failure and its unrecovered
count are appended when failures occur together. The Paper entry point consequently
returns a failure exit status after cleanup.

The start guard checks the terminal latch again after waiting for any previous
producer and writer to join. A concurrent restart cannot pass an early healthy check
and replace the old worker after that worker has latched a write failure.

This is bounded in-process retention, not a durable recovery journal. On owner
destruction or process exit, accepted but uncommitted candles cannot be recovered from
this queue. The final `unrecovered` count and failure diagnostic explicitly report that
history gap. A process crash before persistence has the same boundary. A new process
must not interpret old accepted counters as durable history or fabricate missing
market data. Automated exchange backfill is outside this contract.

## Observable state

`Collector::persistence_snapshot()` is exposed as `collector_persistence` in runtime
status. The error and counters survive `stop()`.

| Field | Meaning |
| --- | --- |
| `status` | `HEALTHY`, `RETRYING`, or latched `FAILED`. |
| `accepted` | Successful queue admissions in this collector lifetime. |
| `persisted` | Admitted items in successfully committed batches; duplicate keys remain one SQLite row. |
| `unpersisted` | Accepted items without a successful commit, including queue and current batch. |
| `unrecovered` | Unpersisted accepted items when terminal failure is latched. |
| `write_failures` | Failed write attempts, independent of queue overflow. |
| `retries` | Retry attempts scheduled after retryable write failures. |
| `rejected_after_failure` | Candles refused after the terminal persistence latch. |
| `queue_overflow_drops` | Admissions refused because the bounded queue was full. |
| `last_sqlite_code`, `last_rollback_code` | Last failed transaction's error classification and rollback result. |
| `failure_reason` | Terminal failure explanation. |
| `writer_active` | Whether the persistence worker is still executing. |
| `drained` | True only after the producer and writer stop, persistence is healthy and every accepted item has committed. |

Counters are observations while producer and writer are active. A committed item
always counts as accepted even if the producer's diagnostic increment has not yet
completed. After both workers join, `accepted = persisted + unrecovered` for a failed collector, or `accepted =
persisted` for a successful drain. Overflow and post-failure refusals were never
accepted and are reported separately. Runtime health is degraded while retrying;
terminal persistence failure stops the run and remains an error after cleanup.

Runtime status also exposes `persistence_healthy` and
`entries_blocked_by_persistence`. New entries require healthy persistence at the
decision boundary, independently of stream freshness. During a retry, existing
positions can still be managed on valid fresh prices. Operator status is `DEGRADED`
while retrying and `HALTED` for terminal failure; `persistence.write_failure` is a
warning or critical alert respectively. The browser shows candle persistence health
separately from socket and market freshness. A live collector with no pending writes
does not claim `drained`: that field certifies completed shutdown only.

## Verification

`collector_persistence_tests` uses real local SQLite transactions and triggers. It
checks whole-batch rollback, blocked-COMMIT rollback and connection reuse, temporary
writer-lock recovery, duplicate UPSERT identity, a permanently locked full queue,
bounded failure shutdown, permanent trigger rejection and concurrent producer-tail
accounting. A failed collector retains its batch and exact unrecovered count, rejects
new intake and refuses restart. The regression runs in Release and the sanitizer
build graph. The runtime qualification runner also requires it for `all-faults` and
`persistence-write-failure` evidence.

`market_stream_recovery_tests` additionally drives the production collector through a
verified local TLS WebSocket into a SQLite trigger rejection. It verifies transport
cancellation, no restart, the retained canonical batch, exact unrecovered accounting,
coordinator failure propagation and complete cleanup. A controlled SQLite statement
gate exercises failure first latched during shutdown and a concurrent restart while
the previous writer is retrying. These fixtures also verify that simultaneous trader,
account and collector diagnostics survive shutdown. No external exchange is used.
