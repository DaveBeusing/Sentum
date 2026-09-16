# Terminal renderer and frame pacing contract

AP-06 separates terminal diff generation and pacing policy from `TerminalUi` so renderer behavior can be tested and benchmarked without writing to a real terminal, while the production terminal consumes the same contract.

## Rendering invariants

The renderer preserves these invariants:

- unchanged frame -> zero terminal payload bytes;
- one changed row -> only that row is addressed, cleared and rewritten;
- removed rows -> stale terminal rows are explicitly cleared;
- resize or workspace switch may request a full redraw;
- full redraws use the alternate-screen viewport reset sequence and are never inferred from ordinary data changes;
- the renderer does not own trading state or mutate runtime state.

`TerminalRenderPipeline.hpp` contains the pure frame-diff function. It returns the terminal payload, current line cache, changed-row count and whether a full redraw occurred.

`TerminalUi::render_frame()` is intentionally thin: it delegates diff construction to the pure pipeline, writes only a non-empty payload, then accepts the returned line cache. The UI-specific regression test captures the terminal stream and verifies that rendering the same frame twice produces no second write.

## Frame pacing

The terminal loop uses `TerminalFramePacer` rather than sleeping for `refresh_` after each iteration.

The pacer uses absolute deadlines. Normal iterations advance from the previous deadline; if poll/render work overruns a deadline, the pacer rebases to `now + interval` rather than issuing catch-up bursts. The terminal therefore avoids making render and I/O duration additive to every refresh period.

This keeps presentation cadence bounded without making UI timing part of trading timing. Market data, strategy, risk and execution paths remain independent from terminal pacing.

## Evidence

`sentum_terminal_render_pipeline_tests` verifies unchanged, single-row, removed-row, full-redraw and frame-pacing behavior.

`sentum_terminal_ui_policy_tests` verifies the production `TerminalUi::render_frame()` integration, including the hard unchanged-frame zero-write contract.

`sentum_terminal_render_pipeline_benchmark` records unchanged-frame and changed-frame diff cost. CI treats timing as **OBSERVED** evidence, while `unchanged_payload_bytes=0` is a correctness requirement.

## Operational boundary

AP-06 changes presentation work only. It does not alter Strategy, Risk, Execution, Position/Trade State or Persistence semantics. Full redraws remain explicit UI events, and the terminal remains a consumer of runtime truth rather than an execution authority.
