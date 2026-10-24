# Account reconciliation and recovery

Sentum treats exchange-confirmed Testnet executions as the authority for positions. Local intent, acknowledgements and strategy signals cannot establish inventory. A successful recovery requires complete owned execution and commission evidence for the configured symbol, a compatible account snapshot, and an authenticated subscription that remains unchanged through publication.

## Recovery evidence

`AccountReconciler::recover` returns verified canonical orders, a chronological execution replay, the restored position, its first confirmed entry time, the latest complete exit time, account permissions and an optional exchange account UID. Its report includes the collection interval and an explicit success or failure. Failed recovery returns no usable position or order replacement.

The recovery procedure is:

1. Keep submission admission closed and retain the existing durable kill latch.
2. Validate a complete tradable Spot account response and the account-wide open-order response. Require explicit base and quote balance rows, including zero balances. Reject duplicate assets, malformed decimals, negative or non-finite amounts, missing permissions, and invalid account identity fields.
3. Resolve every owned order for the configured symbol by its original client identity. Verify symbol, side, requested size, stable exchange identity, cumulative execution and terminal outcome. A locally recorded pre-dispatch or explicit rejection with no exchange execution remains a rejection; an uncertain or missing order does not.
4. Read the symbol's account trade history from `fromId=0`, advancing by the largest returned trade ID plus one. Validate every page and require complete per-trade quantities and commissions, including explicit zero fees. The latest default history window is insufficient.
5. Match every returned trade to a verified locally owned order. An unowned trade, missing persisted trade, changed fee, duplicated trade ID, non-advancing page or incomplete order total keeps recovery blocked.
6. Replay fills in exchange chronology through `ConfirmedPositionLedger`. Partial fills and cancelled partial orders retain exposure. Base fees reduce inventory; quote fees affect acquisition cost or proceeds; third-asset fees retain their asset without an invented conversion.
7. Re-query each exchange order and compare a second account/open-order snapshot. Verify the trade-history tail is still empty. Require the resulting base balance to agree with reconstructed inventory and reject unexplained locked base inventory.
8. Confirm that the authenticated stream and received-event generation have not changed. Commit the verified replacements and replay atomically, then restore the same ledger and strategy position before making readiness visible.

The full proof is scoped to the configured symbol. It does not claim to reconstruct all historical funding or other markets in the account. Funding in quote or unrelated assets may exist independently of this strategy. Current quote and third-asset balances must be well formed, while their unknown original funding is not fabricated into a historical account ledger. Unknown trades of the configured symbol and unexplained base holdings require explicit investigation. Existing Testnet seed inventory is not silently imported into the strategy.

## Open orders and concurrent activity

A successful resume requires all orders on the configured symbol to have an authoritative terminal outcome. Cancellation and query recovery run through the order manager before this proof can succeed. Valid orders on unrelated symbols are not counted as orders of the configured market; changes to account-wide evidence while recovery is running still invalidate the snapshot.

REST account, order and trade endpoints are separate observations. They are not an atomic exchange snapshot. Stable repeated REST reads are therefore combined with the authenticated stream generation/event barrier in the session. A fill or account event arriving during collection or publication invalidates admission until another complete reconciliation. Disconnecting or rebuilding the stream never clears a latch.

## Bounded collection

The default collector reads at most 64 history pages of at most 1,000 trades, followed by an explicit tail check. It checks a 30-second monotonic deadline before and after each exchange request. Reaching a page or time limit means **incomplete evidence**, never a successful truncated recovery.

Cumulative replay is also bounded to 262,144 retained trade/commission entries across its snapshots. This prevents a heavily fragmented single order from creating quadratic retained evidence. Reconstruction and verification loops check the same deadline. Exceeding this memory bound produces an explicit incomplete-recovery result and keeps admission closed.

A request already in flight retains the execution client's transport timeout, currently 15 seconds. The deadline is consequently a collection-admission bound plus at most one in-flight request, not a claim that a blocked HTTP operation can be interrupted at exactly 30 seconds. Limits can be reduced in deterministic fixtures. Large histories that exceed the bounded contract remain blocked until a supported, reviewed recovery procedure is provided.

## Persistent audit and resume

The canonical order repository stores owned snapshots, accepted execution history, explicit reconciliation evidence, kill-switch audit and a process-session guard. Recovery is allowed to replace an incomplete-evidence marker only through the explicit durable reconciliation path; ordinary order updates cannot clear it. The replacement must preserve prior ownership, identities, nondecreasing execution and every already known trade/commission fact.

The process lease and persisted session guard prevent two processes or an interrupted writer from reopening admission. Failed reads, failed proof collection, changed stream state or failed persistence leave submissions blocked. Unbound legacy audit history and credential replacements are not proof of a verified account history.

Explicit resume uses `LiveOrderSession::resume_after_reconcile` with `I_CONFIRM_RECONCILED_TESTNET_RESUME`. It requires a current successful recovery and a durably recorded latch clear. The CLI has no bypass. Clean shutdown also persists the latch, so restart requires the same explicit reconciled resume. Preserve the configured canonical database and its active SQLite companion files when moving or backing up the runtime.

## Restored position protection

Inventory reconstruction cannot recover a previously observed trailing-stop peak from account trades alone. The execution journal therefore also stores a position-policy checkpoint scoped to the credential namespace, symbol and owned opening order. It records the complete risk-policy identity, confirmed quantity/cost, holding start, price extrema and protective levels. A recovered positive position requires compatible durable protection before runtime admission.

Checkpoint updates commit before their new policy state is published. For the same opening order, the risk policy cannot change, the recorded high and stop cannot decrease, the low cannot increase, and the holding start cannot move later. An earlier exchange-confirmed entry time is accepted conservatively. A new owned opening order receives its own protection rather than inheriting the earlier trade's levels. Failed writes retain the previous complete checkpoint and block admission; malformed or missing evidence is not replaced with weaker defaults during recovery.

Favorable price ticks on confirmed partial inventory advance durable protection even while its order is still resolving. The existing order reservation continues to prevent a simultaneous follow-up order. Cancellation must retain its executed quantity, and terminal evidence or completed reconciliation is required before the strategy can submit the next order.

The execution-specific schema is version 2. Its reconciliation proof and position-policy tables are additive; shared SQLite `user_version` and historical order audit rows are preserved. These snapshots do not assert knowledge of market-price extrema that were never observed while the process or subscription was unavailable.

## Exchange rules and execution boundary

Account permissions are returned explicitly for validation against symbol permission sets. `ExchangeRules` combines exchange quantity and notional constraints with the local limits. Rules must be refreshed and validated before submission; an account reconciliation result does not override a trading filter.

`IExecutionVenue` remains the common execution boundary. `BinanceTestnetExecutionVenue` uses Spot Testnet only; simulated venues implement the same request/update contract. No recovery step places a new order to discover the result of an uncertain earlier placement.

## Protocol and regression evidence

The protocol fields and pagination follow the official [Binance Spot Testnet REST account documentation](https://github.com/binance/binance-spot-api-docs/blob/master/testnet/rest-api.md#account-endpoints), especially account information, open orders, query order and the account trade list. `myTrades` with `fromId` returns trades at or after that ID; omitting it returns the recent window. Each request is still subject to exchange rate limits and asynchronous data-source visibility.

`account_reconciliation_tests` exercises malformed and missing account evidence, explicit symbol scope, filled-order restart, fills missed during an outage, partial cancellation, zero/base/quote/third-asset fees, pagination, changed or missing trade evidence, ambiguous order-not-found, ownership conflicts, changing REST snapshots, history limits and collection deadlines. Session integration additionally qualifies atomic durable adoption, stream changes during recovery and strategy restoration before readiness. These fixture checks do not claim credentialed exchange or target-environment acceptance.
