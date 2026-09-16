# Terminal snapshot and rendering contract

Sentum's terminal UI is an operator surface. It must remain responsive without turning dashboard presentation into part of the trading hot path.

## Goals

The terminal architecture must provide:

- coherent dashboard snapshots rather than independently sampled fields;
- no full dashboard JSON copy when state generation has not changed;
- bounded refresh work when the runtime is idle;
- repository reads only for views that actually need persisted history;
- equity sampling only for views that display the equity curve;
- unchanged rendered lines producing no terminal writes where practical;
- resize and operator input forcing deterministic redraws;
- runtime pressure, connectivity, position and entry-state information visible without opening multiple views.

## Versioned snapshot boundary

`DashboardState` now exposes a versioned snapshot contract:

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

This preserves the existing `snapshot()` compatibility API for the web dashboard and other callers while giving the terminal renderer a cheaper polling contract.

## Consistency

A multi-field `merge()` remains atomic from the snapshot reader's point of view. The snapshot generation is captured under the same state mutex as the JSON copy, so the renderer never needs a separate generation read after copying the state.

The regression test repeatedly writes paired fields from one thread while another thread requests only changed snapshots. Torn pairs are a test failure.

## Evidence

`sentum_dashboard_snapshot_benchmark` compares two idle-poll patterns against a representative dashboard payload:

1. unconditional `snapshot()` copying;
2. `snapshot_if_changed()` while the generation is unchanged.

CI records this benchmark as **OBSERVED** evidence. AP-05 does not invent a production SLA from hosted-runner timing.

The correctness requirement is stronger than the timing observation: unchanged conditional polling must report zero snapshot copies.

## Renderer integration contract

The terminal loop should migrate to a cached `DashboardSnapshot` and use the versioned API as its sole runtime-state refresh boundary.

Repository refresh work is view-specific:

- `Orders`: recent orders;
- `Trades`: recent trades;
- `Models`: model records;
- other tabs: no periodic repository query solely because the global two-second interval elapsed.

Equity-curve sampling belongs to the `Market` workspace and should not force repository work or a full redraw of unrelated views.

## Frame pacing

The existing line-diff renderer remains the preferred output model:

```text
cached dashboard snapshot
    -> active workspace render model
    -> frame lines
    -> compare with previous lines
    -> emit changed rows only
```

An unchanged dashboard generation with no operator input, terminal resize or view-specific refresh deadline should result in no render work and no terminal writes.

## Professional terminal priorities

The always-visible header should prioritize operational truth:

- mode and health;
- current symbol and last price;
- strategy;
- entry state (running/paused);
- position state and unrealized P/L when open;
- market-data connectivity;
- persistence pressure when elevated or worse.

Detailed latency, queue and persistence diagnostics remain in the System workspace. Warning states should be visible in the header before the operator navigates there.

## Non-goals

This package does not move trading authority into the UI, change execution semantics, or create new persistence reads on the market producer path. The terminal remains a consumer of runtime truth and operator-control commands.
