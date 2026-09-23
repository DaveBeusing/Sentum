# Terminal snapshot and rendering contract

Sentum's terminal UI is an operator surface. It must remain responsive without turning dashboard presentation into part of the trading hot path.

## Goals

The terminal architecture provides:

- coherent dashboard snapshots rather than independently sampled fields;
- no full dashboard JSON copy when state generation has not changed;
- bounded refresh work when the runtime is idle;
- repository reads only for views that actually need persisted history;
- equity sampling only for the Market workspace;
- unchanged rendered lines producing no terminal writes where practical;
- resize and operator input forcing deterministic redraws;
- runtime pressure, connectivity, position and entry-state information visible without opening multiple views.

## Versioned snapshot boundary

`DashboardState` exposes a versioned snapshot contract:

```text
producer update
    -> DashboardState mutex
    -> mutate one coherent JSON state
    -> increment generation

terminal poll
    -> fast generation comparison
    -> unchanged: no JSON copy
    -> changed: lock once, copy state and generation together
```

`DashboardSnapshot` contains the generation associated with the copied state. `snapshot_if_changed()` performs a lock-free generation check first and only acquires the state mutex when the caller is behind.

The existing `snapshot()` compatibility API remains available for the web dashboard and other callers.

## Terminal integration

`TerminalUi` keeps one cached dashboard JSON snapshot and one associated generation. The loop polls `snapshot_if_changed()` and replaces the cache only when the runtime generation advances. It does not perform a second generation read after rendering.

A render pass is scheduled only for one of these causes:

- dashboard state changed;
- operator input marked the UI dirty;
- terminal width changed;
- the active workspace reached a view-specific refresh deadline.

An unchanged generation with no operator input, resize or active-workspace deadline therefore produces no render work.

## Consistency

A multi-field `merge()` remains atomic from the snapshot reader's point of view. The snapshot generation is captured under the same state mutex as the JSON copy, so the renderer never consumes a generation that does not describe the copied state.

The dashboard snapshot regression test repeatedly writes paired fields from one thread while another thread requests only changed snapshots. Torn pairs are a test failure.

## View-specific refresh policy

Repository refresh work is restricted to the workspaces that render persisted records:

- `Orders`: recent orders;
- `Trades`: recent trades;
- `Models`: model records;
- `Market`, `Scanner`, `Strategy`, `System`: no periodic repository history query.

Changing workspace invalidates the shared repository refresh deadline so the newly selected repository-backed view gets fresh data immediately.

Equity-curve sampling belongs only to the `Market` workspace. Leaving Market stops the periodic equity deadline from forcing redraws in unrelated workspaces.

`terminal_ui_policy_tests` verifies these rules independently of terminal I/O.

## Frame pacing and terminal writes

The line-diff renderer remains the output model:

```text
cached dashboard snapshot
    -> active workspace render model
    -> frame lines
    -> compare with previous lines
    -> emit changed rows only
```

Full-screen redraws occur for startup, explicit workspace changes and terminal resize. Normal runtime updates preserve line-diff output. If the generated frame is identical to the previous frame, the renderer emits no terminal payload.

## Operator header

The always-visible header prioritizes operational truth:

- mode and health;
- current symbol and last price;
- active strategy;
- entry state (`RUNNING` / `PAUSED`);
- persistence pressure;
- equity and realized/runtime P/L;
- unrealized P/L when a position is open;
- market-data connectivity.

Persistence pressure uses the runtime `normal`, `elevated`, `critical` and `saturated` states introduced by the backpressure package. Elevated pressure is yellow; critical or saturated pressure is red. The System workspace also exposes queue high-water, wakeups, pressure transitions and saturation events alongside latency distributions.

The header reports runtime truth only; it does not infer exchange state or invent market data.

## Evidence

`sentum_dashboard_snapshot_benchmark` compares two idle-poll patterns against a representative dashboard payload:

1. unconditional `snapshot()` copying;
2. `snapshot_if_changed()` while the generation is unchanged.

CI keeps dashboard snapshot timing as **OBSERVED** evidence because the hosted-runner measurements show materially higher variance than the scanner and terminal-render benchmarks. No production SLA is derived from that timing.

The correctness requirement is independently **ENFORCED** by the consolidated performance gate: unchanged conditional polling must report zero snapshot copies.

CI also runs dashboard snapshot consistency and terminal refresh-policy tests in Release, ASan, UBSan and TSan configurations.

## Non-goals

This package does not move trading authority into the UI, change execution semantics, or create persistence reads on the market producer path. The terminal remains a consumer of runtime truth and operator-control commands.
