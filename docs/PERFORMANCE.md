# Sentum Core Performance

Phase 8 moves the market-data and simulated execution paths toward an event-driven, bounded architecture.

## Market path

Closed Binance candles follow this path:

```text
WebSocket parse
  -> fixed-capacity MarketDataStore ring buffer
  -> MarketEventBus
      -> SymbolScanner incremental ranking cache
  -> bounded SQLite persistence queue
```

SQLite remains outside the scanner/decision hot path.

`MarketDataStore` allocates each symbol ring buffer once and uses per-symbol locking after map lookup. Return calculations read the ring in place and do not copy an entire candle window.

## Scanner

The scanner maintains cached 30- and 60-sample returns when closed-candle events arrive. A top-symbol change notifies `ExecutionEngine` through a condition variable. The trader trigger no longer wakes on a fixed five-second polling interval.

The console may still read the already-computed ranking once per second for presentation; this does not perform historical-window database or candle copies.

## Incremental indicators

`IncrementalIndicators.hpp` contains bounded/O(1) update primitives for:

- rolling return
- rolling SMA
- EMA
- Wilder-style RSI

`MomentumStrategy` uses `RollingReturn`, avoiding `deque` growth/pop operations and repeated historical scans.

## Unified simulated execution

Paper and replay `TradeEngine` execution is routed through `SimulatedExecutionVenue`, which implements the same `IExecutionVenue` contract as exchange execution.

The simulated venue receives the market timestamp, spread and slippage model, and returns an exchange-style confirmed `FILLED` snapshot. Replay therefore keeps deterministic timestamps while sharing the order/fill boundary with paper mode.

## Benchmark

Build the market-path microbenchmark with:

```bash
cmake -S . -B build-perf \
  -DCMAKE_BUILD_TYPE=Release \
  -DSENTUM_BUILD_BENCHMARKS=ON
cmake --build build-perf --parallel
./build-perf/sentum_market_benchmark
```

It reports:

- total events
- elapsed seconds
- events per second
- nanoseconds per event
- event-bus delivery count
- representative rolling indicator values

The benchmark currently exercises 500 symbols and 1,000,000 events. Treat it as a regression benchmark rather than a claim about exchange-to-order latency.

## Performance acceptance targets

The following targets should be tracked on a stable CI runner or dedicated benchmark host:

```text
persistence queue drop rate        < 0.1%
market event bus delivery          100%
scanner historical DB reads        0
scanner timed polling for entries  0
paper/replay fill implementations  1 shared venue
market path benchmark              no >10% regression without explanation
```

## Remaining hot-path work

The Binance collector still constructs a JSON DOM for each WebSocket message. A future optimization can replace this with an on-demand/SAX parser and interned symbol identifiers after benchmark evidence shows JSON parsing is a dominant cost.
