# Terminal renderer and frame pacing contract

AP-06 separates terminal diff generation and pacing policy from `TerminalUi` so renderer behavior can be tested and benchmarked without writing to a real terminal.

## Rendering invariants

The renderer must preserve these invariants:

- unchanged frame -> zero terminal payload bytes;
- one changed row -> only that row is addressed, cleared and rewritten;
- removed rows -> stale terminal rows are explicitly cleared;
- resize or workspace switch may request a full redraw;
- full redraws use the alternate-screen viewport reset sequence and are never inferred from ordinary data changes;
- the renderer does not own trading state or mutate runtime state.

`TerminalRenderPipeline.hpp` contains a pure frame-diff function that returns the terminal payload, current line cache, changed-row count and whether a full redraw occurred.

## Frame pacing

The legacy loop sleeps for `refresh_` after completing poll/render work. This makes work duration additive to the interval and can cause long-term drift.

`TerminalFramePacer` uses absolute deadlines. Normal frames advance from the previous deadline; if work overruns a deadline, the pacer rebases to `now + interval` rather than issuing catch-up bursts.

This keeps terminal presentation bounded without making UI cadence part of trading timing.

## Evidence

`sentum_terminal_render_pipeline_tests` verifies unchanged, single-row, removed-row, full-redraw and frame-pacing behavior.

`sentum_terminal_render_pipeline_benchmark` records unchanged-frame and changed-frame diff cost. CI treats timing as **OBSERVED** evidence, while `unchanged_payload_bytes=0` is a correctness requirement.

## Remaining integration

The next AP-06 slice replaces the local diff implementation in `TerminalUi::render_frame()` with the pure pipeline and switches the UI loop from relative `sleep_for()` pacing to `TerminalFramePacer` deadlines. Renderer counters can then be surfaced in the System workspace without introducing writes into the trading hot path.
