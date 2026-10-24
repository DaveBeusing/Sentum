# Testnet order validation

The supported Spot Testnet path validates current symbol metadata before enabling
the session and refreshes it before submitting an order. Missing metadata,
malformed values, a halted symbol, unavailable MARKET orders or unsatisfied
account permissions block submission before the placement request. A failed
refresh cannot fall back to older cached rules. Invalidation also rejects a
response that was already in flight when the rules changed.

## Quantity constraints

`ExchangeRules` intersects `LOT_SIZE` and `MARKET_LOT_SIZE`. A zero market-specific
field does not erase an enabled general minimum, maximum or increment. The
combined minimum is the larger enabled minimum; the combined maximum is the
smaller enabled maximum. Both nonzero step constraints apply, so steps of 0.02
and 0.03 produce a common grid of 0.06. Filter order cannot change the result.

Decimal coefficients are retained as integers, including the symbol's base
precision. Approved quantities are converted using their shortest round-trip
decimal representation, then rounded down on the common grid. Normalization
never increases the approved double quantity. Quantities that cannot retain the
same decimal identity through the runtime's double representation are rejected.
The resulting fixed decimal string travels with the approved request and is
serialized without exponent notation or dependence on the process locale.

Local sizing and capital restrictions remain additional constraints. Exchange
metadata cannot authorize an increase beyond the size approved by local risk.
A request below the applicable minimum is rejected; it is never rounded upward
to make the exchange accept it. Selling a quantity that rounds down can leave
exchange dust; such a remainder is still inventory and must not be represented
as a completely closed position.

## Market notional constraints

Each enabled `MIN_NOTIONAL` or `NOTIONAL` bound retains its own `avgPriceMins`.
`applyToMarket`, `applyMinToMarket` and `applyMaxToMarket` determine which bounds
apply to MARKET requests. A supplied last trade price cannot stand in for a
missing weighted-average window.

The exchange's non-null reference price takes precedence over the average-price
window. When no reference price is set, a filter uses the corresponding average
window, or the last price when its window is zero. The session validates price
source, freshness, symbol identity and the returned average interval before
using that evidence. Only the documented `-2043` response establishes that no
reference price has ever been set; unrelated errors do not establish absence.

Both minimum and maximum bounds apply where enabled. Notional comparison uses
decimal arithmetic, while finite positive checks reject a runtime quantity,
price or product that cannot be represented safely. Metadata validation does
not promise order acceptance or a fill price: exchange state can change after
the last observation, and the exchange remains authoritative for execution.

## Numeric input boundaries

All effective risk configuration numbers are validated for finiteness. Risk
budgets, capital caps, quantity increments and resulting approved quantity and
notional must be positive and representable. Fees and slippage must remain
below 100%; spread must preserve a positive sell-side touch. Initial and trailing
stop distances retain their existing percentage constraints. Positive take-profit
targets above 100% remain supported. Time constraints must fit the execution
clock rather than overflow when converted to its duration.

Replay CSV parsing consumes every numeric token and reports the file and line
for malformed values, overflow, non-finite prices or volumes, nonpositive prices
and negative volumes. Missing volume still means zero. Timestamps must fit the
execution clock. Simulated and exchange order boundaries reject invalid numbers
before publishing execution evidence or issuing placement requests. Invalid
simulation setters leave the previous valid price and fill model intact.

## Qualification

`exchange_rule_validation` covers filter permutations, disabled market fields,
intersecting decimal increments, exact quantity strings, malformed metadata,
permission groups, notional flags and price-window semantics, overflow and
cache invalidation during a blocked request.

`numeric_boundary` covers direct and parsed risk configuration, NaN and
infinities, arithmetic overflow and underflow, complete CSV tokens and line
diagnostics, simulated and real order admission, and strategy/position isolation
from invalid prices. `testnet_order_validation` exercises a healthy authenticated
stream fixture and the complete session submission path. It checks fresh
metadata, changed filters, intersecting local and exchange grids, account
permissions, reference/average/last-price evidence, and that failed validation
cannot cross the intent or placement boundary.

## Protocol references

- [Spot Testnet symbol filters](https://github.com/binance/binance-spot-api-docs/blob/master/testnet/filters.md)
- [Exchange metadata and permission sets](https://github.com/binance/binance-spot-api-docs/blob/master/testnet/rest-api.md#exchange-information)
- [Reference-price query](https://github.com/binance/binance-spot-api-docs/blob/master/testnet/rest-api.md#query-reference-price)
- [Current average-price query](https://github.com/binance/binance-spot-api-docs/blob/master/testnet/rest-api.md#current-average-price)

These contracts were checked against the official Spot Testnet documentation on
2026-10-24. Deterministic fixture qualification does not substitute for a
credentialed Testnet session or acceptance in the target environment.
