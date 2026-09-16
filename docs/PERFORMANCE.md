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

Each symbol uses a fixed-capacity ring buffer with per-buffer synchronization. Scanner calculations operate on in-memory data rather than querying SQLite. The scanner is event driven and maintains rankings from completed market updates instead of periodically copying large historical windows.

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

The current market-path gate executes five repetitions for each synthetic universe and evaluates both the median and the worst sample:

| Workload | Events | Median budget | Worst-sample guardrail | Evidence state |
| --- | ---: | ---: | ---: | --- |
| 500 symbols x 2,000 events | 1,000,000 | <= 250 ns/event | <= 400 ns/event | ENFORCED |
| 1,000 symbols x 1,000 events | 1,000,000 | <= 250 ns/event | <= 400 ns/event | ENFORCED |
| 2,000 symbols x 500 events | 1,000,000 | <= 250 ns/event | <= 400 ns/event | ENFORCED |
| parser allocations | 1,000,000 parses | 0 allocations/parse | 0 allocations/parse | ENFORCED |

Every market run must also deliver exactly the number of events it generated. Correctness failure is a performance-gate failure even when timing remains within budget.

The initial gate values were derived from successful GitHub-hosted Release evidence on workflow run `35090140340` (`ubuntu-24.04`, GNU 13.3.0), where the three market-path cases measured approximately 62.10, 77.74 and 63.64 ns/event and the parser reported zero allocations per parse. The enforced limits deliberately preserve substantial hosted-runner headroom; their purpose is to detect material regressions, not normal machine variance.

CI stores `performance_gate.json` and `performance_gate.md` as short-lived workflow evidence and publishes the Markdown result in the job summary.

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
- Release, ThreadSanitizer and long-running Paper soak tests after material concurrency changes
- material market-path regressions fail CI through the repository performance budget gate

Actual throughput and latency depend on hardware, compiler, exchange message rate, symbol universe and enabled strategies.
