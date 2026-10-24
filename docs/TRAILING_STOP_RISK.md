# Trailing-stop risk policy

Paper, replay, research and Testnet use the same long-position exit policy.
The shared evaluator applies fixed stop, trailing stop, take profit and maximum
holding time in the same order. The initial stop uses the confirmed entry fill;
simulated entries include configured spread and slippage, while Testnet entries
use confirmed exchange execution and commission accounting. A favorable price can
tighten the stop, but can never lower it:

```text
new_stop = max(previous_stop, new_high * (1 - trailing_sl_percent))
```

A trailing distance wider than the initial stop is supported. The initial stop
stays in place until the trailing candidate offers stronger protection. For an
entry at 100, a 1% initial stop and a 10% trailing distance, the stop stays at 99
when the price reaches 101 or 110; at 111 it advances to 99.9. Equal highs and
pullbacks do not lower it. Disabling trailing preserves the initial stop.

Stop updates preserve the approved quantity, entry fill and recorded capital at
risk. They do not grant a new risk budget. A stop remains a trigger for a market
exit: when a price gaps through it, the simulated fill uses the observed price
and configured execution costs. The policy does not guarantee that realized
loss, including gaps and fees, is bounded by the nominal entry risk budget.

## Configuration validation

- `stop_loss_percent` must be finite and strictly between 0 and 1.
- `trailing_sl_percent` must be finite and at least 0 but below 1.
- When `trailing_sl_enabled` is true, its distance must be strictly positive.
  An omitted distance is therefore rejected when enabled.
- Disabled trailing permits a missing or zero distance. Negative, nonfinite or
  out-of-range dormant distances are rejected as configuration errors.
- The initial and trailing distances are independent; a wider valid trailing
  distance is not a configuration error.

Validation runs when loading a risk file and when constructing the shared risk
manager. Directly constructed replay configurations and research overrides must
satisfy the same policy before entry decisions or simulated fills are possible.
JSON flags must be booleans and percentage fields must be numbers.

Non-finite prices, quantities, costs and overflowing derived risk values are
rejected before they can create a position or produce a usable approval. CSV replay
also requires complete, finite numeric tokens with valid timestamp bounds.

## Durable Testnet protection

The Testnet runtime persists an account-scoped checkpoint containing the symbol,
owned opening client/exchange order identity, complete risk configuration, entry
time, quantity/cost, price extrema and protective levels. It commits a changed
checkpoint before publishing the new in-memory policy. Failed persistence blocks
admission without publishing weaker replacement protection.

Account trade reconstruction establishes current inventory but cannot infer an
earlier observed market peak. Restoring positive inventory therefore also requires
a compatible checkpoint. Reconnect and process restart preserve the strongest
recorded stop and the original holding interval. Missing, corrupt, mismatched or
stale-position checkpoints keep recovery blocked. A genuinely new owned opening
order receives its own protection; it does not inherit a closed trade's extrema.
Price extremes during an outage that were never observed remain unknown.

## Regression evidence

`trailing_stop_risk_tests` covers wider and narrower trailing distances, repeated
and lower highs, the original stop after a favorable move, exact-stop and gap
exits, confirmed entry costs, disabled trailing, unchanged approval fields,
malformed JSON and direct nonfinite/out-of-range configurations. The tests use
the actual TradeEngine with a deterministic ReplayClock and local in-memory
trade history. `testnet_market_policy_tests` compares actual replay/Testnet exits
and validates restored protection; `testnet_position_policy_tests` exercises
checkpoint persistence failure and position lineage. All groups run with the
normal Release and sanitizer CTest suites.
