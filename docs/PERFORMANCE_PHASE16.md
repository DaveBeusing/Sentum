# Phase 16 - Runtime Performance Hardening

Phase 16 removes the remaining avoidable allocation and locking pressure from the live market-data path while preserving the existing public trading/research interfaces.

## Market-data hot path

The Binance combined-stream collector no longer constructs a `nlohmann::json` DOM for every kline update. `FastBinanceKlineParser` extracts only the required kline fields directly from the WebSocket payload using `std::string_view` and allocation-free decimal/integer parsing.

Required fields:

- symbol
- open timestamp
- OHLC
- volume
- closed flag

Malformed or incomplete messages are ignored without entering the trading path.

## Symbol IDs

At collector construction all configured markets are interned once. Each symbol receives a compact `SymbolId` and a stable canonical string. Incoming symbols are resolved with a case-insensitive FNV-1a hash plus collision verification.

`MarketDataStore` supports direct `SymbolId` access, so collector and scanner events do not repeat string-hash lookups in the normal market path. The string representation remains on `MarketEvent` for compatibility with UI, logs and persistence.

## SQLite queue

The former mutex-protected `std::deque` between Collector and SQLite writer has been replaced by a fixed-capacity single-producer/single-consumer ring queue.

Properties:

- 8192 usable entries
- no queue growth
- no producer mutex in the normal path
- bounded memory
- existing drop counter and drop-rate behavior retained
- queue high-water mark recorded

SQLite remains on its dedicated writer thread and receives batches of up to 256 items. Batch entries hold pointers to stable interned symbol strings, avoiding a symbol allocation/copy on queue insertion.

## Runtime latency histograms

`RuntimePerformanceMetrics` records bounded histograms for:

- parser latency
- market-event dispatch latency
- SQLite batch latency

Snapshots expose:

- count
- average microseconds
- p50
- p95
- p99
- maximum

The Paper runtime publishes these under the Dashboard `performance` object together with:

- `market_events_total`
- `events_per_second`
- `queue_depth`
- `queue_high_water`
- `drop_rate`

## Runtime housekeeping

Dashboard state is merged once per runtime refresh instead of issuing a sequence of individual lock-taking `set()` calls. Database file-size probing is reduced from once per second to once every 15 seconds.

## Exchange metadata cache

Binance Spot Testnet exchange rules are cached for 60 minutes in `ExchangeMetadataCache`. Repeated quantity/notional validation therefore does not require repeated `/api/v3/exchangeInfo` requests. The cache can be invalidated explicitly when exchange rules need to be refreshed.

## Benchmarks

Build:

```bash
cmake -S . -B build-perf -DCMAKE_BUILD_TYPE=Release -DSENTUM_BUILD_BENCHMARKS=ON
cmake --build build-perf --parallel
```

Market path:

```bash
./build-perf/sentum_market_benchmark 500 2000
./build-perf/sentum_market_benchmark 1000 1000
./build-perf/sentum_market_benchmark 2000 500
```

The first argument is symbol count and the second is events per symbol.

Parser allocation test:

```bash
./build-perf/sentum_parser_allocation_benchmark
```

Acceptance is zero heap allocations during one million calls to `FastBinanceKlineParser::parse()` after benchmark setup.

## Phase-16 acceptance targets

- normal Binance kline parsing does not build a JSON DOM
- parser benchmark reports zero heap allocations per parse
- Collector -> SQLite handoff uses a bounded fixed SPSC ring
- SQLite remains outside the market decision path
- `SymbolId` is available through Collector, MarketEvent and MarketDataStore
- p50/p95/p99 runtime latency is observable
- queue depth/high-water/drop rate are observable
- dashboard runtime updates are batched
- filesystem size polling is no longer performed each second
- exchange-rule metadata is cached
- market-path benchmark supports at least 500, 1000 and 2000 simulated symbols

The phase intentionally does not replace the legacy Console UI; that is reserved for the unified CLI/TUI phase.
