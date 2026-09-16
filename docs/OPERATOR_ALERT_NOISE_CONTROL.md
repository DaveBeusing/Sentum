# Operator Alert Attention, Suppression & Noise Control

## Purpose

AP-19 applies a presentation-only attention policy to the shared Operator Alert Center so terminal and web surfaces can reduce low-value alert noise without hiding safety-relevant alerts.

## Authority boundary

Noise control never acknowledges, resolves or clears an alert. It does not mutate runtime state, control-plane state, Risk, Execution, fills, kill-switch state or entry state. Every derived view keeps `execution_authorized = false`.

## Severity rule

`CRITICAL` and `WARNING` alerts are never suppressed by the attention policy. They remain visible even when acknowledged or when the low-priority alert budget is exhausted.

`ATTENTION` and `INFO` alerts are eligible for presentation suppression when:

- a low-severity alert has already been acknowledged;
- a low-severity alert is cleared and retained only as lifecycle evidence;
- the configured low-priority presentation budget has been exhausted.

Suppression means "not expanded as a visible Alert Center row". It does not delete lifecycle evidence. Suppressed totals remain visible in terminal and web summaries.

## Flapping

A currently active alert with lifecycle generation `>= 3` is presented as `FLAPPING`. Flapping low-severity alerts remain visible even when the regular low-priority presentation budget is exhausted. This makes repeated clear/reactivate cycles explicit instead of silently suppressing them.

Generation is presentation evidence supplied by the alert lifecycle contract; the attention policy does not increment generations itself.

## Storm control

The Alert Center accepts a bounded low-priority budget. Once that budget is exhausted:

- additional `ATTENTION`/`INFO` rows are suppressed;
- `storm_limited` becomes true;
- the number of suppressed alerts remains visible;
- all `CRITICAL`/`WARNING` alerts stay visible;
- active flapping alerts stay visible.

This rule prevents low-severity alert storms from consuming the terminal or browser surface while preserving important operating signals.

## Cross-surface contract

The `/api/operations` `alerts` object exposes:

- `suppressed`;
- `flapping`;
- `storm_limited`;
- per-item `attention`;
- per-item `flapping`;
- per-item `attention_reason`.

Terminal and web consume the same `OperatorAlertCenterView`; no browser-side reclassification or suppression algorithm exists.

## Regression expectations

Tests verify:

- Critical and Warning are never suppressed;
- low-severity storm budget is bounded;
- acknowledged and cleared low-severity alerts are deprioritized;
- flapping low-severity alerts remain visible;
- terminal and web expose identical suppression/flapping state;
- noise control never grants execution authority.
