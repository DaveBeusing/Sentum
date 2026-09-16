# Runtime observability and backpressure

## Goal

Runtime observability must make saturation and latency visible without becoming a material part of the market-data hot path itself.

The optimized runtime therefore separates high-frequency observation from lower-frequency aggregation:

```text
market event
  -> sampled latency observation
  -> bounded persistence queue push
       -> push result already carries depth + empty transition
       -> pressure state updated only from observed queue state
       -> writer wakeup only on empty -> non-empty
  -> background writer / dashboard consume aggregated metrics
```

## Latency sampling

Parser, market-event dispatch and strategy-decision latency use deterministic 1:64 sampling.

This removes two `steady_clock` calls plus histogram atomic updates from 63 of every 64 hot-path invocations while retaining a continuous operational latency distribution.

SQLite batch latency remains fully measured because batches execute at a much lower frequency and are operationally important for persistence backpressure.

Sampled latency histograms are **OBSERVED** evidence. Their percentile values describe sampled runtime behavior; they are not production SLAs.

## Queue observation without duplicate reads

`SpscRingQueue::try_push_observed()` returns:

- whether the item was accepted;
- whether the queue was empty before the push;
- the resulting queue depth.

Those values are calculated from the producer-side head/tail state already required to perform the push. The collector therefore does not perform a second `size_approx()` read merely to update telemetry.

## Writer wakeups

The writer condition variable is notified only when a successful push transitions the queue from empty to non-empty.

Additional pushes while the writer already has visible work do not generate redundant wakeup syscalls. Shutdown still uses `notify_all()` so lifecycle semantics remain bounded and deterministic.

`queue_wakeups` records the number of data-path wakeups independently from shutdown notifications.

## Pressure states

Persistence backpressure is represented as four operational states:

| State | Queue occupancy |
| --- | ---: |
| `normal` | below 50% |
| `elevated` | 50% to below 80% |
| `critical` | 80% to below capacity |
| `saturated` | push rejected because the queue is full |

The exact queue capacity remains 8,192 items. Pressure-state changes are edge-triggered; repeated events in the same state do not increment the transition counter.

Runtime metrics expose:

- `queue_depth`
- `queue_high_water`
- `queue_pressure`
- `queue_pressure_level`
- `queue_pressure_transitions`
- `queue_saturation_events`
- `queue_wakeups`

Drops remain fail-visible through the collector's accepted/dropped counters and drop rate. No item is silently overwritten to reduce pressure.

## Thread-safety contract

The persistence queue remains SPSC: one market-data producer and one database writer.

Queue-pressure state is atomic because producer and writer both update the observable pressure after queue changes. Histogram counters remain relaxed atomics because they are diagnostic aggregates rather than synchronization primitives.

Sampling state for parser/dispatch is collector-owned and only used by the WebSocket producer thread. Strategy latency uses thread-local sampling state so independent execution threads do not contend on the sampler.

## Regression coverage

`runtime_observability` verifies:

- observed SPSC push depth;
- empty-to-non-empty detection;
- full-queue behavior and wrap-around;
- deterministic latency sampling cadence;
- sampled histogram count;
- queue pressure, transition, saturation and wakeup accounting.

The suite runs in normal Release CI and under ASan, UBSan and TSan.

Existing AP-02 performance budgets remain authoritative. A telemetry optimization is not accepted by weakening those budgets.
