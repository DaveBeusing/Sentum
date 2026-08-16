# Runtime performance

Sentum is designed so market-data ingestion, strategy decisions and simulated/exchange execution do not block on persistence or dashboard work.

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

## Performance acceptance goals

Performance should be evaluated with measurable criteria rather than absolute claims tied to one machine:

- no SQLite blocking in the market-to-decision path
- bounded persistence memory usage
- measurable queue drop rate with an explicit operational threshold
- parser and decision p99 latency visible at runtime
- no routine heap-allocation hotspot in kline parsing
- stable behavior under 500, 1,000 and 2,000-symbol synthetic benchmark universes
- Release, ThreadSanitizer and long-running Paper soak tests after material concurrency changes

Actual throughput and latency depend on hardware, compiler, exchange message rate, symbol universe and enabled strategies.
