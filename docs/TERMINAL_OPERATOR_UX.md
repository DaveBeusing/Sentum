# Terminal operator workspace and UX contract

AP-07 defines the operator-facing information hierarchy for Sentum's terminal UI. The terminal remains a presentation and control surface; it does not own trading authority or reinterpret execution truth.

## Operator priority

The terminal should answer these questions in order:

1. Is the runtime safe and healthy?
2. Is market data connected?
3. Is persistence under pressure?
4. Are new entries allowed?
5. What symbol, position and strategy are active?
6. Which workspace contains the next diagnostic detail?

Warnings must not compete with decorative information. A critical or warning condition must be visible before the operator navigates to a detailed workspace.

## Severity model

`TerminalWorkspacePolicy.hpp` provides a deterministic presentation policy:

- `NORMAL`: runtime healthy, market connected, persistence normal, entries running;
- `ATTENTION`: startup, elevated persistence pressure or intentionally paused entries;
- `WARNING`: market disconnect or critical/saturated persistence pressure;
- `CRITICAL`: kill switch active or runtime health outside healthy/starting states.

This is a UI severity model only. It does not change Risk, Execution or RuntimeControl behavior.

## Workspace contract

The terminal keeps seven stable workspaces and numeric shortcuts:

- `1 MARKET` — position, price, risk and market watch;
- `2 SCANNER` — candidate ranking and watchlist;
- `3 ORDERS` — execution and order-state history;
- `4 TRADES` — realized trade history and P&L;
- `5 STRATEGY` — signal, confidence and risk decision;
- `6 MODELS` — model lifecycle and promotion state;
- `7 SYSTEM` — latency, queue, persistence and runtime health.

Workspace numbering is part of the operator muscle-memory contract and should remain stable unless intentionally versioned.

## Recommended workspace

Each non-normal operator state may point to the workspace that contains the relevant detail:

- kill switch / paused entries -> `MARKET`;
- runtime health / connectivity / persistence pressure -> `SYSTEM`.

The recommendation is navigational guidance only and must never auto-change trading state.

## Next integration slice

The next AP-07 slice should integrate the policy into the always-visible terminal header and warning area, replacing ad-hoc status composition with one consistent severity banner. It should also normalize workspace labels/help text and keep the existing zero-write renderer contract from AP-06.

## Non-goals

AP-07 does not add order placement, model promotion, risk overrides, or execution authority to the terminal UI. All such actions remain behind their existing control and service boundaries.
