# Public Market Stream Recovery

The combined kline collector and the per-symbol public trade stream share one transport implementation, `ReconnectingMarketStream`. A live worker is distinct from an open socket and from current usable market data. Neither thread creation nor a successful WebSocket handshake establishes market-data readiness.

## Connection ownership and recovery

Each attempt creates and initializes a private WebSocket++/Asio endpoint before publishing it for cancellation. One owning worker runs that endpoint. A close, failed handshake, connection deadline, or prolonged absence of valid data ends the attempt. The stopped endpoint and its pending handlers are destroyed before another attempt begins. Old handlers cannot publish state into a new endpoint.

Retries use an interruptible exponential delay starting at 250 milliseconds and capped at 10 seconds. An attempt that delivered a valid event resets the delay. Retry count is not capped: a network outage keeps the worker available for recovery without a busy retry loop. Stop latches cancellation, wakes backoff immediately, stops the current I/O context, and prevents a late handshake from publishing a connected state after cancellation.

The default connect deadline is 5 seconds. The default freshness budget is 2 seconds. A connected socket that has not delivered a valid event for `max(10 seconds, 5 × freshness budget)` is replaced. The idle check also applies to a connection that has never delivered its first valid event. A fresh endpoint is required for an explicit start after stop.

The connection deadline bounds application I/O processing. Platform DNS resolution remains an operating-system dependency: Boost.Asio can join a background resolver executing `getaddrinfo` while an endpoint is destroyed. A stalled OS resolver can therefore extend endpoint teardown and full shutdown beyond the application deadline. The local qualification suite proves cancellation during connect/TLS handshake, active delivery and backoff; it does not claim a hard real-time bound for the operating-system resolver or for exchange availability.

TLS certificate-chain and hostname validation remain required on every attempt. Recovery does not relax trust requirements. The production collector and trade endpoints are unchanged. Local endpoint overrides are accessible only through test fixtures, not runtime configuration.

The owner must stop and join the worker before destroying the transport. Client callbacks may request stop; the owning thread performs the join and endpoint destruction. Start/stop serialization preserves this rule even when a callback requests stop during an external join. Collector producer completion still precedes persistence-writer completion.

## Market-data freshness

`MarketStreamHealth` reports these independent fields:

| Field | Meaning |
| --- | --- |
| `worker_running` | The owning transport worker remains available, including during backoff. |
| `connected` | This attempt completed its WebSocket handshake and has not observed close/failure/cancellation. |
| `fresh` | The current connection delivered a valid event whose age is within the configured budget. |
| `last_event_age_ms` | Exchange event age at receipt plus elapsed monotonic time; `-1` means no valid event was received. |
| `last_event_time_ms` | Exchange epoch timestamp of the last accepted valid event. |
| `connection_attempts` | Attempts initiated during this explicit client lifetime. |
| `successful_connections` | Completed WebSocket handshakes. |
| `reconnects` | Successful connections after the first one. |
| `state` | `connecting`, `backoff`, `waiting_for_data`, `live`, `stale`, or `stopped`. |

Closing a socket clears connection readiness immediately. Reconnection cannot reuse a previous connection's freshness. Invalid payloads, non-positive/non-finite prices, out-of-range integer epochs, events older than the freshness budget, and event timestamps more than one second in the future do not refresh health or produce price callbacks. Integer epochs are checked before conversion to the platform's `system_clock` duration.

For the public trade stream, `T` is the exchange trade time and becomes the Paper `MarketEvent.timestamp`. Prices are no longer assigned a new event timestamp simply because they arrived at the local queue. Queue age checks therefore reject old buffered events.

For kline updates, `E` supplies the update time used for freshness; `t` remains the candle identity/start persisted to SQLite. A freshly closed one-second candle may have a start older than a short freshness budget. Using the update time prevents that candle from being incorrectly classified as stale solely because of its interval. The `E` parser uses checked, allocation-free integer conversion; the separate broader numeric-parser validation work remains independent.

The Paper collector and its price client use the configured risk `max_data_age_ms`. Accepted budgets are positive and no greater than 24 hours, preventing duration overflow. This upper limit is a representability guard, not a recommendation to trade on day-old prices.

Protocol field meanings follow the [Binance Spot WebSocket Streams specification](https://developers.binance.com/docs/binance-spot-api-docs/web-socket-streams).

## Decision and position behavior

New Paper entries require a fresh collector and a fresh per-symbol price stream. An entry-readiness guard is evaluated at the trading decision; the one-second dashboard refresh is not the authority for enabling entries. Operator pause remains independent and is never cleared automatically by a reconnect.

An existing simulated position remains open during an outage. The engine does not invent a fill, close at a cached quote, or reset strategy/position ownership merely because the transport reconnects. It resumes stop, target and holding-time evaluation on a fresh valid price. A stop crossed during the gap is filled using that resumed simulated market price under the existing spread/slippage model; the configured stop price is not a guaranteed execution price. Risk exits remain available from fresh per-symbol prices while the collector is still unavailable for new entries.

Public-stream recovery also applies to the client used by Testnet. It updates public market connectivity and freshness without changing authenticated user-stream readiness, order reconciliation, kill-switch authority or the separate Testnet endpoint-selection findings.

## Runtime and operator views

Paper publishes `collector_market_stream` and, when a trader exists, `price_market_stream`. Aggregate `market_data_connected` and `market_data_fresh` require both relevant streams. `entries_blocked_by_market_data` records the entry guard; `last_market_event_age_ms` is the worst relevant event age or `-1` when a required source has no valid event.

The existing operational policy reports `DEGRADED` for disconnected data and for an open-but-stale stream. The browser shows socket connectivity and freshness separately. The alert policy emits `market.stale` for a connected stream without current valid events. Runtime process health, socket health and data freshness are deliberately separate measurements.

## Qualification

`market_stream_recovery_tests` runs the actual production clients against a local TLS WebSocket server with a temporary generated CA and a verified `localhost` identity. It covers:

- initial connection refusal followed by endpoint availability;
- first connection remaining unready until a valid message;
- a connected but silent stream becoming stale and being reconnected;
- abrupt socket loss followed by data delivery on the same client instance;
- collector reconnect and persistence drain on the same collector instance;
- hostname mismatch rejected on every retry;
- bounded local cancellation during a pending handshake and during long backoff;
- invalid/old/extreme event timestamps and invalid freshness configuration;
- blocked new entries during collector unavailability;
- an open simulated position surviving an outage and resuming its stop on a fresh gap price;
- operator degraded status and stale-data alerts.

The `market-data-reconnect` runtime-qualification scenario now closes a real local TLS transport, observes disconnected health, reconnects the same `Collector`, and verifies all 192 received candles in SQLite. It no longer substitutes two manually constructed persistence writers for a network reconnect. The new tests are part of Release CTest and the existing sanitizer dependency graph. They make no credentialed exchange request and do not establish live-exchange soak evidence.

## Design decision

A shared transport keeps timeout, retry, cancellation and freshness semantics consistent between the combined collector and per-symbol stream. Separate retry implementations were rejected because they had already diverged in lifecycle and health behavior. Thread ownership stays with the existing runtime components; the transport does not create another autonomous control plane or take position/execution authority. The principal trade-off is that reconnect repairs future delivery without reconstructing market events missed during the outage. Historical gap backfill and OS-resolver hard deadlines require separately scoped work if those guarantees become necessary.
