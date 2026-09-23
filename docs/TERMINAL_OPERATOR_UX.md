# Terminal operator workspace and UX contract

terminal operator UX defines the operator-facing information hierarchy for Sentum's terminal UI. The terminal remains a presentation and control surface; it does not own trading authority or reinterpret execution truth.

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

## Presentation contract

terminal operator UX exposes presentation-ready text from the same policy that determines severity and workspace guidance:

- `operator_banner_text()` renders one deterministic operator banner;
- `workspace_navigation_text()` renders the stable numeric workspace map and marks the active workspace;
- `workspace_help_text()` supplies the active workspace purpose from the canonical descriptor table.

This avoids duplicating severity wording and workspace labels in multiple terminal rendering paths. Regression tests verify normal/warning banner text, active-workspace marking, shortcut presence and workspace help text.

The production `TerminalUi` should consume these presentation helpers in its always-visible header/navigation area. Doing so must preserve terminal frame-pacing's unchanged-frame -> zero terminal output bytes contract.

## Non-goals

terminal operator UX does not add order placement, model promotion, risk overrides, or execution authority to the terminal UI. All such actions remain behind their existing control and service boundaries.
