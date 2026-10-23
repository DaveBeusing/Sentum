# Trailing-stop risk policy

Paper, replay and research use the same long-position stop policy in `TradeEngine`.
The initial stop is calculated from the confirmed simulated entry fill, including
the configured spread and slippage. A favorable price can tighten that stop, but
can never lower it:

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

These guarantees cover the shared simulated TradeEngine path. Testnet trailing
policy parity is a separate execution-runtime contract; this change does not
implement Testnet trailing exits or qualify exchange order recovery.

## Regression evidence

`trailing_stop_risk_tests` covers wider and narrower trailing distances, repeated
and lower highs, the original stop after a favorable move, exact-stop and gap
exits, confirmed entry costs, disabled trailing, unchanged approval fields,
malformed JSON and direct nonfinite/out-of-range configurations. The tests use
the actual TradeEngine with a deterministic ReplayClock and local in-memory
trade history. They run with the normal Release and sanitizer CTest suites.
