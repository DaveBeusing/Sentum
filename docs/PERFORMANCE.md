# Runtime performance

Sentum is designed so market-data ingestion, strategy decisions and simulated/exchange execution do not block on persistence or dashboard work.

## Performance evidence states

Performance claims use three evidence states:

- **ENFORCED**: a reproducible CI gate has a defined budget and fails when the budget is exceeded;
- **OBSERVED**: the runtime or a benchmark exposes the metric, but no stable pass/fail threshold is established yet;
- **UNVERIFIED**: no current evidence supports a pass/fail or production-SLA claim.

A metric must not be promoted from OBSERVED or UNVERIFIED to ENFORCED without repeatable measurements on the intended evidence environment. CI regression budgets are not production latency SLAs.

## Market-data path

The runtime path is:

```text
Binance WebSocket
    -> FastBinanceKlineParser
    -> SymbolId / MarketEvent
    -> fixed MarketDataStore ring buffers
    -> MarketEventBus
    -> scanner / strategy

                         -> fixed SPSC persistence queue
                         -> SQLite WAL writer
```

The normal Binance kline parser extracts only the fields required by Sentum instead of building a complete JSON DOM. Symbols are interned to compact `SymbolId` values for hot-path storage and scanner access; strings remain at UI, logging and persistence boundaries.

## Persistence

Closed candles are passed to a bounded single-producer/single-consumer ring queue. The SQLite writer owns the database write path, reuses prepared statements, uses WAL mode and performs batched writes. The trading decision path does not wait for SQLite.

Queue depth, high-water mark and drop rate are observable at runtime. The queue is bounded by design so load cannot produce unbounded memory growth.

## In-memory market store

Each symbol uses a fixed-capacity ring buffer with per-buffer synchronization. Scanner calculations operate on in-memory data rather than querying SQLite.

The `SymbolId` hot path uses an immutable ID-to-buffer snapshot published during symbol registration. Normal ID-based reads and writes therefore do not acquire the global symbol-map `shared_mutex` and do not increment a per-symbol `shared_ptr` reference count on every event. The underlying ring buffer remains lifetime-owned by the store's symbol map.

When the scanner needs both 30- and 60-sample cumulative returns, `MarketDataStore::cumulative_returns()` computes both from one consistent ring-buffer snapshot while holding the per-symbol mutex once. String-based lookup remains available as a compatibility path for events without a valid `SymbolId`.

## Scanner hot path

The event-driven scanner maintains compact return state by `SymbolId`. The normal valid-ID path no longer hashes and scans the full symbol-return map for every market event.

For 30-sample top-symbol selection, Sentum maintains an ordered incremental ranking:

```text
closed candle
    -> one ring-buffer lock for 30/60 returns
    -> update only the changed SymbolId cache entry
    -> erase/insert one ordered ranking entry
    -> read current top from ranking begin()
```

The update cost is therefore O(log N) for the ranking rather than an O(N) full-market top search on every closed candle. Dashboard/top-N reads may still perform broader collection/sorting because they are not on the market producer hot path.

Events without a valid `SymbolId` retain the string-keyed compatibility path; that path is not treated as the optimized normal runtime route.

## Runtime telemetry

`RuntimePerformanceMetrics` tracks:

- total market events and events per second
- parser latency
- event-dispatch latency
- strategy/risk/execution decision latency
- SQLite batch latency
- persistence queue depth and drop rate

Latency distributions expose average, p50, p95, p99 and maximum values. The terminal System view and web dashboard use these metrics for operational visibility.

These runtime latency histograms are currently **OBSERVED** metrics. They are useful for hotspot identification and soak evidence, but the repository does not claim a production p99 SLA for them yet. A later workload-specific qualification step must establish such limits from measured evidence rather than inventing them.

## Dashboard overhead

Runtime UI data is published through batched `DashboardState` snapshots. The web dashboard uses independent read-only SQLite connections for historical data. The terminal console caches trade/order/model reads instead of reopening SQLite on every redraw. Database-size probing is rate limited rather than performed every runtime tick.

## Exchange metadata

Symbol filters and exchange rules are cached with a TTL. Quantity, notional and precision validation therefore does not require an exchange-info request for every order decision.

## Benchmarks

Build performance targets with:

```bash
cmake -S . -B build-perf \
  -DCMAKE_BUILD_TYPE=Release \
  -DSENTUM_BUILD_BENCHMARKS=ON
cmake --build build-perf --parallel
```

Market-path scaling examples:

```bash
./build-perf/sentum_market_benchmark 500 2000
./build-perf/sentum_market_benchmark 1000 1000
./build-perf/sentum_market_benchmark 2000 500
```

The parser-allocation benchmark performs repeated kline parsing and checks the hot parser path for heap allocations:

```bash
./build-perf/sentum_parser_allocation_benchmark
```

A healthy optimized build should report zero allocations per normal parser invocation.

The scanner benchmark exercises the production `MarketDataStore -> MarketEventBus -> SymbolScanner` path after a 60-candle warm-up per symbol:

```bash
./build-perf/sentum_scanner_hot_path_benchmark 500 200
./build-perf/sentum_scanner_hot_path_benchmark 1000 100
./build-perf/sentum_scanner_hot_path_benchmark 2000 50
```

Scanner timing is now **ENFORCED** as a hosted-runner regression budget. The promotion is based on five successful GitHub-hosted Release runs on `ubuntu-24.04` with GNU 13.3.0. The measured run-to-run variation was low enough to establish separate median and worst-sample guardrails for all three universe sizes. These limits are CI regression controls, not production latency SLAs.

## Enforced CI performance gate

`benchmarks/performance_budgets.json` is the controlled CI budget definition. `tools/ci/performance_gate.py` executes the Release microbenchmarks, validates benchmark correctness, evaluates the budgets and writes machine-readable evidence.

Run it locally with:

```bash
python3 tools/ci/performance_gate.py \
  --build-dir build-perf \
  --budget-file benchmarks/performance_budgets.json \
  --report log/performance_gate.json \
  --summary log/performance_gate.md
```

The gate evaluates the following repository-controlled budgets:

| Metric / workload | Median budget | Worst-sample guardrail | Correctness requirement | Evidence state |
| --- | ---: | ---: | --- | --- |
| market path: 500 x 2,000 | <= 250 ns/event | <= 400 ns/event | exact generated/delivered event count | ENFORCED |
| market path: 1,000 x 1,000 | <= 250 ns/event | <= 400 ns/event | exact generated/delivered event count | ENFORCED |
| market path: 2,000 x 500 | <= 250 ns/event | <= 400 ns/event | exact generated/delivered event count | ENFORCED |
| parser allocations | n/a | n/a | 0 allocations/parse | ENFORCED |
| scanner: 500 x 200 | <= 375 ns/event | <= 450 ns/event | exact event count and top-count contract | ENFORCED |
| scanner: 1,000 x 100 | <= 390 ns/event | <= 475 ns/event | exact event count and top-count contract | ENFORCED |
| scanner: 2,000 x 50 | <= 420 ns/event | <= 525 ns/event | exact event count and top-count contract | ENFORCED |
| dashboard unchanged snapshot polling | OBSERVED | OBSERVED | 0 conditional snapshot copies | timing OBSERVED / correctness ENFORCED |
| terminal unchanged frame | <= 1,050 ns/frame | <= 1,300 ns/frame | 0 payload bytes | ENFORCED |
| terminal changed frame | <= 1,300 ns/frame | <= 1,600 ns/frame | changed frame emits payload | ENFORCED |

Market, scanner and terminal timing gates execute repeated measurements and evaluate both median and worst-sample behavior. Correctness failure remains a gate failure even when timing is within budget.

The original market/parser budgets were derived from successful GitHub-hosted Release evidence on workflow run `35090140340` (`ubuntu-24.04`, GNU 13.3.0), where the three market-path cases measured approximately 62.10, 77.74 and 63.64 ns/event and the parser reported zero allocations per parse.

Scanner, dashboard and terminal qualification used five additional successful Core CI runs: `35201427700`, `35204003032`, `35207600347`, `35788383555` and `35791460185`, all on `ubuntu-24.04` with GNU 13.3.0. Scanner measurements stayed within approximately 291-314 ns/event (500 symbols), 316-325 ns/event (1,000 symbols) and 319-352 ns/event (2,000 symbols). Terminal frame-diff measurements stayed within approximately 849-905 ns/frame unchanged and 1,030-1,107 ns/frame changed. The enforced guardrails retain deliberate hosted-runner headroom above those observed ranges.

Dashboard timing was not promoted: unchanged conditional polling ranged from roughly 24 to 47 ns/poll across the same runs, and unconditional snapshot cost also showed materially greater runner variance. Its stronger invariant is therefore enforced as a correctness budget: unchanged conditional polling must produce zero snapshot copies.

CI stores `performance_gate.json` and `performance_gate.md` as the consolidated short-lived performance evidence and publishes the Markdown result in the job summary. The summary labels ENFORCED and OBSERVED dimensions explicitly.

## Runtime qualification and lifecycle evidence

The runtime qualification harness contributes commit-bound RSS, thread-count, queue and sampled-latency evidence to the performance report when its JSON artifacts are available.

RSS growth/trend and sampled parser/dispatch/strategy latency remain **OBSERVED** in the performance budget model. The qualification harness still applies its separate bounded RSS-growth guardrail and fails incomplete evidence, but that guardrail is not reinterpreted as a production memory SLA.

A whole-runtime startup timing budget is not currently promoted because normal Paper startup includes configuration, exchange metadata and network-facing initialization that is not a deterministic hosted-runner benchmark boundary. Shutdown has deterministic component-level lifecycle assertions, including bounded queue draining and dashboard stop behavior, but the repository does not convert those functional lifecycle limits into a whole-system production shutdown SLA. A dedicated startup/shutdown timing budget should be added only after an isolated deterministic workload is available and repeatable evidence supports it.

## Budget change policy

Performance budgets are code-reviewed control values. Changing them requires evidence.

- Tightening a budget should cite repeatable measurements showing the tighter limit is stable.
- Loosening a budget must explain the measured environmental or product reason and must not be used merely to make a regression green.
- A failed gate should first trigger hotspot/regression analysis.
- Hardware-specific production qualification must use separate workload evidence rather than silently reinterpreting these GitHub-hosted CI limits as production SLAs.
- Benchmark workload shape, compiler mode and measurement statistics are part of the contract; changing them is a performance-evidence change, not housekeeping.

## Performance acceptance goals

Performance should be evaluated with measurable criteria rather than absolute claims tied to one machine:

- no SQLite blocking in the market-to-decision path
- bounded persistence memory usage
- measurable queue drop rate with an explicit operational threshold
- parser and decision p99 latency visible at runtime
- no routine heap-allocation hotspot in kline parsing
- stable behavior under 500, 1,000 and 2,000-symbol synthetic benchmark universes
- scanner event cost does not contain an unconditional full-market top search
- valid `SymbolId` store access does not require the global symbol-map lock
- Release, ThreadSanitizer and long-running Paper soak tests after material concurrency changes
- material market-path regressions fail CI through the repository performance budget gate

Actual throughput and latency depend on hardware, compiler, exchange message rate, symbol universe and enabled strategies.
