# Durable Testnet order state

## Context and decision

An order submission can reach the exchange even when its HTTP response never
reaches Sentum. Exchange reports can also arrive before the placement response,
more than once, or after a cancellation response. The order manager therefore
owns one canonical record for each locally submitted order. It persists the
record before exposing a transition to downstream position accounting.

The execution boundary preserves three independent facts: local ownership,
exchange-confirmed cumulative execution, and whether another submission is
permitted. A transport error cannot erase ownership or confirmed execution, and
reconnecting a stream cannot clear the entry-blocking latch.

This decision puts persistence in the order/session boundary. The runtime's
observer audit remains useful for diagnostics, but it cannot provide the
write-before-submission guarantee because observers run downstream of the venue.
Replaying only the latest open orders also loses completed and partially executed
orders, so accepted local history is retained separately.

## Identity and monotonic state

`client_order_id` remains the original locally reserved identity. Exchange order
IDs are scoped by symbol. A cancellation's replacement client ID is recorded as
an alias after the original client ID and exchange ID have been validated.
Unrelated account reports do not acquire local ownership by naming the same
symbol. Conflicting identity or side information blocks further submission.

The supported states include `unknown`, which records an unresolved outcome.
Confirmed cumulative quantity and quote value cannot decrease. Delayed `NEW`
reports and placement acknowledgements cannot weaken a newer terminal state.
Duplicate reports do not create another execution. A late commission report may
enrich the existing record without reverting its order state.

Cancellation responses and execution reports pass through the same identity and
state checks. A complete fill racing cancellation remains a complete fill. A
cancelled or expired order retains every confirmed partial execution.

## Submission uncertainty

The reserved request is durable before the placement call. A validated explicit
exchange rejection can finish that request as `rejected`. A timeout, transport
failure, malformed response, ambiguous exchange error or server error keeps the
outcome unresolved and latches order entry.

Recovery queries use the same original client order ID. They never place another
order to discover whether the first one succeeded. An empty open-order list, or
a temporary order-not-found response, does not establish that an ambiguous
placement was rejected. The persisted identity survives process restart.

Authoritative recovery can establish a fill, a partial execution or a terminal
cancellation. Resolving the outcome does not automatically clear the kill switch.
REST order summaries establish cumulative execution but do not establish complete
commission history. New execution recovered from a summary, or a stream gap in
the trade/fee evidence, sets a durable `requires_reconciliation` marker. Confirmed
gross exposure remains visible, while further submission and explicit resume stay
blocked until the full account-recovery contract can verify the missing history.

## Confirmed inventory and commissions

`ConfirmedPositionLedger` tracks an applied cumulative watermark per owned
exchange order. Each accepted snapshot contributes only the new execution and
commission deltas. Replaying the same accepted history produces the same result.

Partial buys immediately contribute confirmed inventory and their acquisition
cost. Cancellation or expiry preserves that inventory. Partial sells reduce the
existing quantity by their executed amount and leave the remainder under position
management. Only a fully closed position resets its entry state.

Commission identity is the exchange trade ID within its canonical order. Base
asset commissions reduce inventory; quote asset commissions contribute to
acquisition cost or reduce sale proceeds. Third-asset commissions remain recorded
in their own asset, without an invented currency conversion. Base and quote asset
names come from explicit exchange metadata. Missing or contradictory fee evidence
requires reconciliation.

The strategy uses the same cumulative accounting rules. It retains the active
order reservation until the order is terminal and retains the submission
reservation until the venue call returns. Observer callbacks continue to run
outside order-manager and strategy state locks.

## Durable latch and recovery boundary

The real session requires a durable order repository. Its namespace is bound to
the Testnet API-key fingerprint; credentials themselves are not stored. Canonical
snapshots, accepted history and kill-switch transitions have separate durable
records. The schema is additive to the existing runtime database.

The canonical journal uses `config/config.json`'s `databasePath`, with
`log/sentum.sqlite3` as the configuration fallback. The strategy's separate
diagnostic observer still writes `log/klines.sqlite3`. Legacy-history detection
examines the configured canonical database; it does not search other database
paths automatically. A path change must preserve and review the previous journal
and audit evidence. Selecting an empty database does not establish reconciliation
of the exchange account.

An execution session also writes a durable active-session guard. An interrupted
session or failed latch write cannot make the next process assume that order entry
was allowed. The operating-system execution lease prevents two processes from
controlling the same namespace concurrently.

Clearing the latch requires successful reconciliation, no unresolved order,
successful durable audit of the clear, and the explicit
`I_CONFIRM_RECONCILED_TESTNET_RESUME` confirmation through
`LiveOrderSession::resume_after_reconcile`. Starting the process or rebuilding the
stream does not clear it. The CLI currently has no override for this confirmation
boundary.

A clean shutdown also durably latches order entry. An ordinary restart therefore
requires the same explicit, successful reconciliation and resume as a recovered
session. Clean shutdown closes the process-session guard; it does not grant the
next process permission to trade.

Legacy `order_events` are retained. Their old audit-only schema does not contain
enough ownership, cumulative quote or commission evidence to prove a complete
recovered account state. Such evidence is not silently promoted to canonical
history. Databases with unbound historical evidence and credential replacements
within an existing journal therefore require verified recovery before order entry
can resume.

## Account recovery and qualification boundaries

The session combines these order-state guarantees with a bounded account/trade
proof and a subscribed event barrier. It commits canonical recovered executions
and reconciliation evidence atomically, restores the ledger and strategy, and
validates durable position protection before exposing readiness. Unknown activity,
incomplete history or missing protection keeps the session blocked. The complete
scope and limits are documented in [Account reconciliation](ACCOUNT_RECONCILIATION.md).

The authenticated WebSocket API subscription, immediate disconnect admission
barrier and explicit Testnet market feed are described in
[Testnet runtime](TESTNET_RUNTIME.md). Fixture and loopback TLS qualification
does not constitute a credentialed Testnet acceptance run.

## Protocol references

The implementation follows the documented uncertain-outcome and execution-report
contracts in the official Binance Spot API sources:

- [General REST API information](https://github.com/binance/binance-spot-api-docs/blob/master/rest-api.md): server errors and timeouts can leave execution status unknown and require a status query.
- [Error codes](https://github.com/binance/binance-spot-api-docs/blob/master/errors.md): `-1006` and `-1007` have unknown execution outcomes.
- [Testnet User Data Stream](https://github.com/binance/binance-spot-api-docs/blob/master/testnet/user-data-stream.md): original cancellation identity `C`, exchange order ID `i`, cumulative execution `z`/`Z`, trade ID `t`, and commission `n`/`N`.

## Regression evidence

The order-state, durability and execution-delta regressions exercise real manager,
repository, ledger and strategy code with deterministic exchange fixtures. They
cover ordering races, unknown outcomes, cancellation aliases, partial execution,
commission replay, process restart, failed SQLite writes and explicit resume.
The existing synchronous-callback regression remains part of this boundary. All
groups participate in the Release, ASan, UBSan and TSan CTest graph.
