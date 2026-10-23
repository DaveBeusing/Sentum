# Trade finalization and recovery

## Decision and scope

Paper and replay use `TradeEngine` with `SimulatedExecutionVenue`. A simulated exit is confirmed synchronously, while its trade-history projection can encounter an independent SQLite writer or a storage failure. An exception after that fill must not leave the position open, repeat the sale, or apply profit twice.

The implementation uses a bounded writer reservation, an immutable close identity, a separate durable SQLite recovery journal, and a transactional history projection. This contract applies to simulated closes. Exchange order/fill reconciliation remains owned by the Testnet execution path.

## Finalization sequence

1. Reserve the main history writer with `BEGIN IMMEDIATE` before submitting the simulated exit. The normal busy timeout is five seconds. An exhausted reservation fails before the exit is submitted.
2. Receive the confirmed simulated fill and build the complete closed position, including its exit timestamp, fees, profit and a unique finalization identifier.
3. Save the immutable full closed-position payload in the recovery journal using SQLite `synchronous=EXTRA`. This adds the directory synchronization required by rollback-journal deletion; in WAL mode it has the same durability behavior as `FULL`.
4. In the reserved main transaction, insert the existing `trades` row and a `trade_finalization_commits` identity/payload marker. Commit them together.
5. Remove the handled pending record from the recovery journal.
6. Publish the complete closed position and apply its profit, win/loss count, last-exit time and completed-trade entry once. SELL evidence uses this closed state and the confirmed exit timestamp.

If steps 3–5 fail after a confirmed fill, the engine still records one coherent closed state in memory and latches the failure before accepting another decision. The runtime stops, joins its workers, completes account settlement where storage permits, and reports the failure. It does not automatically launch a replacement trader after that worker error.

SQLite `COMMIT` can leave a transaction active after a busy error; other errors can roll it back automatically. Finalization cleanup checks the transaction state and rolls back any remaining main transaction before releasing ownership.

## Files and compatibility

The existing `trades` columns and existing rows are preserved. The additive `trade_finalization_commits` table binds each new finalization identifier to its complete immutable payload and trade row. Reusing an identifier with different content is an error.

For a file-backed main database, the recovery journal is stored beside its canonical path:

```text
<history-database-path>.trade-finalizations.sqlite3
```

The journal contains `pending_trade_finalizations`; its serialized payload has an explicit version and preserves the complete closed position, including event timestamps. Constructor failure preserves unresolved or corrupt recovery records for diagnosis. Corrupt, unsupported or inconsistent payloads prevent startup instead of creating replacement history.

An in-memory history database uses an in-memory journal. It has the same transactional and identity checks within the process, but no durability across process termination. Research output retains its existing deterministic event/metric contracts; the internal recovery identifier is not a trading decision or a metric input.

## Recovery states

| Failure point | Durable authority | Result |
| --- | --- | --- |
| Main writer reservation cannot be acquired | No exit has been submitted | Stop with a bounded contention error. The current position has not been changed by a sale. |
| Confirmed fill cannot be written to the journal | The current process retains the confirmed closed snapshot; durable recovery is not established | Stop with an explicit unrecordable-close diagnostic. Reconcile the retained evidence before restarting. |
| Journal commit succeeds; history insert or commit fails | Pending journal payload | Reopening retries the history projection once storage is repaired. |
| History commit succeeds; journal cleanup fails | Main history and its identity marker, plus the pending journal entry | Reopening recognizes the matching committed identifier and removes the pending entry without another history row. |
| Recovery payload is malformed or conflicts with a committed identity | Existing files remain available | Refuse startup and report the inconsistency. |

Recovery only projects an already recorded confirmed close. It does not submit another order. Repeated retries and repeated reopening must leave one history row for the same finalization identifier.

## Operational handling and limits

Keep the history database and its recovery journal together when moving or restoring a runtime. Quiesce writers before taking a coordinated backup and use SQLite-aware backup/restore procedures, including any active SQLite journal/WAL files. Deleting the recovery database to bypass a startup error can discard the only durable copy of a close.

After a finalization failure, repair the reported storage problem and inspect the history, pending recovery record and Paper account before resuming. The recovery journal's path appears in the error. A history cleanup failure does not mean the history transaction failed; the reported failure classification distinguishes those states.

The Paper account snapshot is a separate durable transaction. History recovery does not automatically reconcile the account's profit or completed-trade count after an abrupt process exit. That cross-store reconciliation remains a separate requirement. Within a running engine, the confirmed close is applied once even when history projection fails.

There is no atomic transaction encompassing the in-process simulated fill and the first journal write. A process failure in that interval, or storage that cannot record the journal at all, cannot be presented as durably recoverable. The implementation reports a detected journal failure explicitly and halts; it does not claim to repair missing evidence after total storage loss. SQLite durability also depends on the filesystem honoring its locking and synchronization operations.

## Alternatives considered

- A busy timeout alone handles ordinary writer contention but leaves failure after a confirmed fill unresolved.
- Writing history before simulating an exit would record execution that has not been confirmed by the venue.
- Updating only the in-memory position before persistence prevents a duplicate sale in that process but loses the recovery record on restart.
- An independent SQLite journal retains confirmed close evidence when the main history write fails. It adds a second file and a cleanup/replay protocol, with immutable identifiers preventing duplicate projection.

## Validation and future changes

`trade_finalization_tests` covers a competing writer, busy-budget exhaustion before a fill body, blocked COMMIT, automatic rollback, immutable-identity conflicts, legacy rows, journal failure and corrupt recovery. Subprocess fixtures terminate without destructors after a failed history write or a failed cleanup, then verify recovery and a second reopening without duplicate history. The same tests verify final SELL state and historical holding duration.

Any later retention policy must preserve the relationship between history rows and identity markers for the required replay window. Any account-reconciliation implementation must consume stable finalization identities and distinguish history repair from account settlement. A future durable simulated-execution ledger could close the fill-to-first-journal crash interval and should replace this limitation explicitly rather than assuming it is already covered.
